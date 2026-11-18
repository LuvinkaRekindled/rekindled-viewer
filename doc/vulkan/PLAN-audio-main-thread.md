# Plan: FMOD main-thread cost (until the backend changes)

Written 2026-10-21. Nothing here is built. Related: the backend replacement question (miniaudio or OpenAL Soft) is a separate task; this plan keeps its fixes backend-neutral where it can.

## Goal and measured starting point

Make audio cost the main thread as little as possible with FMOD, without an audio thread.

| What | Measured | Source |
|---|---|---|
| Steady state `idle: audio` | 0.04-0.06 ms/frame | `run-logs/rtshadows-cpuzones-20261019-*.log`, `cpuzones.log` |
| Load burst (one 600-frame window, about 14 s after an avatar rez) | **4.00 ms/frame average = about 2.4 s of audio in 14 s**, inside a 9 ms `idle()` | `run-logs/s9-range3.log`, 2026-10-21 00:57:10 |
| Same kind of window, Oct 19 | 3.18 ms/frame | `rtshadows-cpuzones-20261019-135349.log` |
| Device-change hitch | one 0.4-0.8 s main-thread stall per launch or device change (FMOD `update()` calls our device-list-changed callback, which calls `setDriver`/`getDriverInfo`, a Windows audio-service RPC) | `run-logs/rtshadows-20261011-124531.log` (stall logger) |

Targets: audio at most 0.5 ms/frame averaged over any 600-frame window including a sim entry or teleport, no audio frame over 2 ms, and device changes off the main thread.

Cause of the burst is **not proven**. Best guess: `LLAudioData::load()` -> `LLAudioBufferFMODSTUDIO::loadWAV()` (blocking `createSound`, file open + sample build) runs on the main thread from `LLAudioEngine::idle()` for every sound a new sim wants. Stage 0 decides.

## Rules for this work

- Launcher + login only; Adam does the teleport himself (never drive the viewer GUI).
- One build + one batch of runs per stage; instrument two levels deeper than the question (`feedback-instrument-two-levels-deeper`).
- Every behaviour change behind an `FS...` setting so one build A/Bs live.
- Fresh short session per stage; separate commit per stage; commit only audio files.
- Do not touch anything the backend swap will delete unless it is the FMOD flag itself. No audio thread.

## Stage 0: measure (about half a day, one test cycle)

Add timing that `llaudio` can report (it cannot use `RK_CPU_ZONE`, which lives in newview `rkgpuzonetimer.h`): a small stats struct in `llaudio` (accumulated microseconds and counts, reset on read), read and logged by the existing `RK_CPU_ZONES` block in `llappviewer.cpp` (next to `idle: audio`, line about 6485) under the `CpuZones` tag.

Timers (per frame average and max, per 600-frame window):
- `LLAudioEngine::idle` parts: source loop (`update` + `updatePriority`), channel pick + `play`, `updateChannels` (it runs twice per idle), queued-sound loop, sync loop, `LLAudioDecodeMgr::processQueue`, `startNextTransfer`, `updateInternetStream`.
- `LLAudioData::load` split into `getFreeBuffer`, `fileExists`, `loadWAV` (`createSound`), plus bytes loaded.
- FMOD `System::update()` in `LLListener_FMODSTUDIO::commitDeferredChanges`, and `setListener`/`updateWind` in `llvieweraudio`.

Counters (log every 10 s): sources in `mAllSources`, sources muted, channels playing, buffers in use, loads per second, **buffer evictions per second** (`getFreeBuffer` taking over an old buffer; the cap is `LL_MAX_AUDIO_BUFFERS` 60), channel steals.

Test: run `run-viewer-clean-rtshadows-cpuzones.bat`, Adam logs in, teleports to a busy sim, crosses a region, stays 60 s, closes normally. Keep the log as `run-logs/audio-stage0-*.log`.

Decision gate: whichever timer owns the burst picks the next stage. Expected outcomes:
- `loadWAV`/`createSound` dominates -> Stage 1.
- Source loops dominate -> Stage 2.
- Evictions per second high -> Stage 3 first (the churn is re-loading the same sounds).

## Stage 1: asynchronous sound loading (about 1 day)

Setting `FSAudioAsyncLoad` (default on once verified).

Generic engine (backend-neutral, survives the backend swap):
1. `LLAudioBuffer`: add virtual `isReady()` (default true) and `checkLoaded()` (default no-op).
2. Gate on `isReady()` at: `LLAudioEngine::idle` channel-candidate test (`llaudioengine.cpp:280`), `LLAudioSource::setupChannel` (`:1486`), and `LLAudioSource::update` (`:1423-1432`, so a pending buffer does not call `play()` every frame, which would spam the NaCl sound-explorer log via `logSoundPlay`).
3. Poll pending buffers once per `idle()` (throttle to every few frames if the timers say so); cap loads started per frame.
4. `getFreeBuffer` must skip buffers that are still loading (it currently deletes the oldest unused one).
5. Move the failure path of `LLAudioData::load` (remove the `.dsf`, drop the buffer, re-preload, mark failed) so it also runs when `checkLoaded()` reports an error.

FMOD part (`llaudioengine_fmodstudio.cpp`): `FMOD_NONBLOCKING` in `loadWAV`'s mode, `isReady()` / `checkLoaded()` via `Sound::getOpenState`, `getLength()` returns 0 until ready and `playSynced` guards the modulo.

Open FMOD questions to settle in the first test (check the 2.03.12 docs and the log): does `getOpenState` work with `FMOD_INIT_THREAD_UNSAFE` (otherwise drop the flag), does `Sound::release` block on a loading sound, and what `playSound` returns on a not-ready sample.

Also in this stage (cheap, removes more main-thread work): skip `fileExists` before `loadWAV` when `hasDecodedData()` is true (the decode worker already wrote the file); keep a few spare buffers created in advance so the engine does not allocate in the burst.

## Stage 2: source and channel loops (about half a day, only if Stage 0 shows them)

Setting `FSAudioSourceUpdateInterval`.
- Update priorities for a rotating slice of sources per frame (muted, far and idle sources less often); keep every source that has a channel at full rate.
- `updateChannels()` runs twice per `idle()`: run the second pass only for channels whose source changed.
- Change detection in `LLAudioChannelFMODSTUDIO`: do not re-send `setVolume`, `setMode`, and 3D attributes when gain, loop flag and position/velocity are unchanged (small epsilon on position).
- Throttle `LLStreamingAudio_FMODSTUDIO::update()` (open-state and tag polling) to about 4 Hz.

## Stage 3: buffer churn (about half a day, only if evictions are high)

If Stage 0 shows many evictions per second: raise or make adaptive the buffer cap (RAM cost per buffer is the decoded PCM), prefer evicting buffers whose sound has no live source, and avoid re-creating a buffer for a sound that was evicted a moment ago. Report buffer memory in the stats line.

## Stage 4: device changes off the main thread (about 1 day, riskier)

Setting `FSAudioDeviceWorker`.
- Replace the work done inside `systemCallback` (`set_device`, `getDevices`, `OnOutputDeviceListChanged`) and `LLAudioEngine_FMODSTUDIO::setDevice` with a flag + a job on a worker. FMOD runs with `FMOD_INIT_THREAD_UNSAFE`, so calling it from another thread needs that flag removed (cost: a lock per API call, about 500 calls/frame, expected negligible; measure with the Stage 0 timers).
- Keep `gAudioDeviceMutex` semantics (voice shares it; FIRE-36022 was a hot-unplug crash with a USB headset). Keep the COM init on the main thread as it is.
- Test: Bluetooth headset off/on, default device change in Windows, choosing a device in Preferences, voice on and off during a change.

## Verification for every stage

- Same route and launcher for before and after; compare the Stage 0 numbers: audio ms/frame averaged over 600-frame windows during the load window, total audio ms in the first 30 s after the teleport, steady state, and the longest single audio frame.
- Correctness checklist: sounds play after a teleport, UI sounds, looping object sounds, queued sounds and loop sync, a deliberately corrupted `.dsf` (delete one mid-run), mute and unmute, the sound-explorer floater log has no repeated play entries, stream start/stop, device switch in Preferences.
- Keep logs as `run-logs/audio-stageN-*.log`.

## Order and effort

0 (0.5 day) -> decision -> 1 (1 day) -> re-measure -> 2 and 3 only if the numbers ask for them (0.5 day each) -> 4 (1 day, can be done any time after 0). Total about 2-3 days if everything is needed.

## Out of scope

An audio thread; replacing FMOD; wind generation (already on the FMOD mixer thread); voice. The backend replacement (miniaudio or OpenAL Soft) is its own plan; Stage 1's engine-level gating is written so that plan can reuse it.
