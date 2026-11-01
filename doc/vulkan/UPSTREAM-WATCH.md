# Upstream watch: the weekly look at three repositories

native-vk is a Firestorm release with a different renderer, so three upstreams matter, each for a different reason. This is the policy, the weekly procedure and the log of what was decided, so nothing is decided twice.

## The three sources and what we take from each

| source | what we take | how |
|---|---|---|
| **Linden Lab viewer** (`secondlife/viewer`, branch `develop`) | **Bug and crash fixes only.** No features, no additions: those wait until they reach a Firestorm release. | Most fixes wait for Firestorm to carry them: Firestorm has many beta testers and releases slowly, so waiting is a free buffer of testing. Cherry-pick ahead of Firestorm only a crash or memory-safety fix in code we still run, on its own `upstream/ll-<date>` branch, one at a time, each with Adam's go. Note the SHA in the log below so the later Firestorm merge is expected to carry it. |
| **Firestorm releases** (`FirestormViewer/phoenix-firestorm`, tags `Firestorm_Release_*`) | Anything relevant: fixes, updates and features. | **Release tags only** (not `master`, not the betas). One release at a time, merged on `upstream/fs-<version>`. Read the digest's overlap list and dry-run conflicts first. After the merge: build, an avatar lap, a plaza frame rate lap, a texture animation lap, then into native-vk. These merges are the careful ones: much of what they touch (avatar, object, volume, texture list, spatial partition, `pipeline.cpp`) is code we changed. |
| **Aurora** (`Aurora-Viewer/Aurora-Viewer`, Rust on wgpu, branch `main`) | **Ideas and measurements only.** It is GPL-3.0 and Firestorm is LGPL: never copy code. | Read what direction they are going. A useful idea becomes a numbered entry in `PLAN-aurora-ideas.md` and is measured before anything is built (no LOD or visual quality cuts, as before). |

## The weekly procedure

1. **The digest** (read-only; also what the scheduled task runs on Mondays): `python <work>\phoenix-firestorm\doc\vulkan\upstream-watch\upstream-check.py` writes `upstream-watch/reports/<date>-digest.md` from the reviewed pointers in `upstream-watch/state.json`. It fetches the Firestorm tags, asks GitHub for the other two, and dry-runs the merge of a new Firestorm tag with `git merge-tree`. It changes no branch and no working file.
2. **The triage** (a session, with me): read the digest and write `upstream-watch/reports/<date>-triage.md` with three lists: *take now* (small and safe), *take with the next Firestorm merge*, *Aurora ideas*. LL fixes are sorted by the digest's hint: files nobody on our side touched apply as they are; files we changed lightly need the merge checked; files we changed heavily (`pipeline.cpp`, `llvoavatar.cpp`, `llvovolume.cpp`, the GL render files) are read for the intent, since the code may be one we replaced.
3. **Adam decides** what is taken. Nothing is applied from a report without his go.
4. **Log it** in the section below, then `python ...\upstream-check.py --advance` moves the reviewed pointers to the heads of the digest, so the next week starts after them.

Pointers (`state.json`): `ll.reviewed_sha`, `fs.reviewed_tag` (reviewed, not necessarily merged; `fs.merged_tag` is what is in native-vk), `aurora.reviewed_sha`, and `our_base` (the Firestorm commit our changes are counted against for the "against our changes" column: change it to the new tag after a Firestorm merge).

## Log

### 2026-11-01: baseline and first look

- **Baselines set** to today's heads: LL `373addb1c` (develop), Firestorm `Firestorm_Release_7.2.5.81788` (reviewed, **not merged yet**; `merged_tag` is empty), Aurora `f234fd51e` (main).
- **Firestorm 7.2.5** (46 commits on our base): dry-run merge has 3 conflicts (`llimage.cpp`, `llimage.h`; `avatarSkinV.glsl`, where we keep our version: it already clamps the palette index, rejects NaN weights and fixes the joint 14 spike). The Flat UI (LL 26.2) with the `ansastorm_modern` skin and the Inter font is already in our base; 7.2.5 is the first tag that ships it. Worth reading after the merge: the jelly doll and animesh change (`llvoavatar.cpp`: `isRlvSilhouette`, `isTooComplex`, `isImpostor`, `getOverallAppearance`), the Windows commit-memory pressure in `llviewertexture.cpp` (`isSystemMemoryLow/Critical/Recovered`), the ExtraParams crash fix in `llviewerobject`.
- **LL fixes looked at ahead of Firestorm**, none taken yet (each waits for Adam's go, on the base-update branch):
  - `d67326e62` heap-use-after-free of the `LLDrawInfo` texture matrix: `LLVOVolume::processUpdateMessage` dirties the group and marks a rebuild when a texture animation is added or removed (two places). Check against the scene table and the GPU texture animation first.
  - `bcb90c8dc` (#6530) `LLMotionController::updateMotionsMinimal` keeps the animation clock running while the pose work is skipped: applies to the far-avatar tier.
  - `fa8967bb6` `set_thread_name` passes `sizeof(ULONG_PTR)` to `RaiseException` (a stack read overflow): one line; our `llthread.cpp` needs a look.
  - `373addb1c` (#6568) mSkinInfo lifetime: we already hold it in a `LLConstPointer` (the Beq defense); the null check is in GL code we do not have. Nothing to port.
  - `6256` (open, unreviewed pull request): skin weights normalized once on the CPU at buffer fill. Idea only; a small possible saving in the skinning paths (our vertex path and the ray tracing skinning).
- **Not LL fixes to take now:** the Chat Modernization project (server-side DM history, a Chat Service) and the web Join and Privacy floaters are features for a later Firestorm release; the async inventory cache (#6419) could shorten cold logins: watch for it in a Firestorm release.
- **Aurora, a first skim of its last 29 commits** (not yet triaged): `perf(render): texture memory lowers once and holds after a teleport (#1802)`, `fix(render): background texture reads no longer overflow their thread's stack (#1806)`, `feat(render): motion vectors for GTAO's history (#1702)`, `docs(render): the 4K shading measurements and their dead ends in TASKS (#1842)`. The first one is about the texture memory budget (our `updateClass`), the third about what we do for the AO history under the upscaler. They go in the first real triage.

### 2026-11-01 (later): Firestorm 7.2.5 merged into native-vk

- `upstream/fs-7.2.5` (merge commit `6c03b94140`) fast-forwarded into native-vk. Conflicts as predicted: `llimage.cpp` / `llimage.h` keep both sides, `avatarSkinV.glsl` keeps ours. `state.json`: `merged_tag` and `our_base` are now `Firestorm_Release_7.2.5.81788`.
- **LL fixes taken ahead of Firestorm** (commit `3ea5c9eab3`; expect them again in the Firestorm release that carries LL 26.5 fixes, where they should merge as already applied): `d67326e62` (rebuild the group when a texture animation is removed: two places in `llvovolume.cpp`), `bcb90c8dc` (#6530, `updateMotionsMinimal` keeps the animation clock), `fa8967bb6` (`set_thread_name` size). Not taken: `373addb1c` (already covered by the `LLConstPointer`).
- A crash seen on the first merged lap (the moved list pass read a freed drawable) is the one of plan 8.31, not a merge effect: the same code crashed before the merge. Diagnostic in (`e7ff11f744`); the watch for `DrawableTrace` lines in normal sessions' logs is open.
- The next Firestorm release is merged the same way: tag only, `upstream/fs-<version>`, the digest's overlap and dry-run first.
