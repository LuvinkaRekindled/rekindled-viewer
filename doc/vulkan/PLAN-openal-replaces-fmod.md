# Plan: OpenAL Soft replaces FMOD, with the audio work off the main thread

Written 2026-11-02. Nothing here is built. Goal (Adam): FMOD gone before release. Supersedes the "no audio thread, FMOD only" rule of `PLAN-audio-main-thread.md`: that plan kept its fixes backend-neutral for this swap; the measurements in it still stand.

## Where things are (read 2026-11-02)

- `indra/llaudio/llaudioengine_openal.cpp` (737 lines) and `lllistener_openal.cpp` (110) exist and are the LL/Firestorm fallback backend. `llstartup.cpp` picks FMOD first and OpenAL only when FMOD is not compiled in (or `LL_BAD_FMODSTUDIO_DRIVER` on non-Windows). `USE_OPENAL` is OFF in our build; no `openal32.dll` in Release. `autobuild.xml` lists openal-soft 1.24.2-r1 for windows64/linux64/darwin64 (secondlife/3p-openal-soft); `Copy3rdPartyLibs.cmake` already copies `openal32.dll alut.dll`.
- OpenAL backend has: channels/sources/buffers, `playSynced` (AL_SEC_OFFSET), 3D position/velocity, loop detection by sample offset, listener doppler/rolloff, wind (main thread queues 50 ms buffers), output device list and switching (`ALC_ENUMERATE_ALL_EXT`, `alcReopenDeviceSOFT`, `ALC_SOFT_system_events`).
- It uses ALUT (`alutInit`, `alutCreateBufferFromFile`, `alutExit`): unmaintained.
- Streaming: `createDefaultStreamingAudioImpl()` returns null, so `llstartup.cpp` uses `LLStreamingAudio_MediaPlugins` (libvlc/cef plugins). Its metadata path (`updateMetadata`) already exists.
- FMOD-only work we did that has no OpenAL twin: non-blocking loads, lost-channel handling, change detection, device-change handling off the callback (see `PLAN-audio-main-thread.md`).
- Voice (WebRTC) and the media playout in `llwebrtc_winplayout` do not touch this.

## Does audio need the main thread at all?

No. OpenAL Soft's API is thread safe and it mixes on its own thread. What ties audio to the main thread is only the engine layer: `LLAudioEngine::idle` reads viewer objects (positions, priorities) and 135 `gAudiop->` call sites in newview call into it. So the split is:

- **Main thread keeps** the game-side state (`LLAudioEngine`, `LLAudioSource`, `LLAudioData`, priority and channel assignment). Steady state it costs 0.04-0.06 ms a frame.
- **Audio thread owns** the `ALCdevice`/context and every AL object: buffer creation from decoded data, source parameters, play/stop, wind, device changes and enumeration, shutdown.
- **Main to audio:** a per-frame snapshot (listener position/velocity/orientation, master gain, doppler/rolloff) plus a command list (play, stop, set buffer, gain, loop, 3D position, sync) in a lock-free ring; applied in order each audio tick (about 10 ms) inside `alcSuspendContext`/`alDeferUpdatesSOFT` so a frame's changes land together.
- **Audio to main:** a small per-channel state block (playing, sample offset, looped this tick) written by the audio thread, read by main next frame. `isPlaying` / `updateLoop` are the only reads; one frame of latency is fine if a channel with a play command pending counts as playing.
- **Loading:** `LLAudioDecodeMgr` already decodes on workers. The WAV read plus `alBufferData` moves to the audio thread (or a worker), so the 3-4 ms/frame load burst and the file open leave main. A buffer is "not ready" until the audio thread says so (the `isReady`/`getLength` guard from the FMOD plan applies).
- **Device:** `getDevices`, `setDevice`, the system-events callback and the preference-panel list refresh run on the audio thread, result posted to main. Removes the 0.4-0.8 s device-change stall.

One path after the move: a temporary switch `FSAudioThread` (and `LL_AUDIO_BACKEND=openal|fmod` while both backends exist), then delete the losing paths.

## Stages

**0. Build and play (half a day, needs Adam's go for the package download).** `autobuild install` the openal package (a download from GitHub; ask first), `USE_OPENAL=ON`, `LL_AUDIO_BACKEND=openal` picks it at startup. Log in, check the log lines (version, device), sounds, wind, device switch, mute, quit. Measure `idle: audio` with `FS_CPU_ZONES` against FMOD on the same lap (stage 0 timers from the old plan are still wanted for the load burst).

**1. Drop ALUT.** Read the WAV the decoder wrote (16-bit PCM, mono/stereo, the header we write ourselves), `alBufferData`; `alcOpenDevice`/context directly. Small, behaviour-neutral. Clears the "ALUT missing" risk and the `alut.dll`.

**2. Heavy things off main (the bulk of the gain).** Audio thread with device, context, buffer upload, device list/switch/events, shutdown. Main keeps issuing the per-frame source updates directly (OpenAL Soft is thread safe) behind a mutex for the first cut, so the risk is small. Measure the load burst and a forced device change.

**3. Wind.** Generate on the audio thread into `AL_SOFT_callback_buffer` (1.22+) so the mixer pulls it, or a streaming source fed by the audio thread. Same `LLWindGen`; the main thread only sets target frequency/gain/pan. Listen for gaps during a main-thread stall.

**4. Commands and snapshot (the full split).** Replace the direct per-frame AL calls with the command ring and the state block. Measure `idle: audio` and the main thread's total AL time; the expectation is small (0.05 ms) so judge by structure: no AL call on main at all, no lock shared with the mixer. Keep it only if it measures neutral or better; otherwise stop at stage 2.

**5. Streaming radio.** Test `LLStreamingAudio_MediaPlugins` with this build's libvlc plugin: start/stop, volume, pause, title/metadata tags (Firestorm's "now playing"), stream reconnect, a multi-hour stream. Decide what to do with the stream buffer-size preference (`supportsAdjustableBufferSizes` is false on the plugin path).

**6. Parity checks.** Priority and channel limits (`LL_MAX_AUDIO_CHANNELS` 30; OpenAL Soft default 256 sources, fine). Volume and distance falloff compared by ear and by number (AL_INVERSE_DISTANCE_CLAMPED vs the FMOD curve; the rolloff factor). Doppler. Mute/unmute, minimise (FMOD needed the window handle), device hot-plug, no-device start, sample rate (OpenAL Soft may open at 48 kHz), resampling quality (`AL_SOFT_source_resampler` replaces `FMODResampleMethod`), HRTF off by default. Sounds that loop, UI clicks, sound-sync of linked objects.

**6b. HRTF (found 2026-11-02, Adam: the beach sounded more real).** OpenAL Soft turns HRTF on by itself for a headphone output; mono world sounds are placed in 3D with it, which FMOD did not do, at a lower level than the plain panning. Keep it as a feature: a setting (auto / on / off) in Preferences > Sound, applied with `ALC_HRTF_SOFT` in the context attributes or `alcResetDeviceSOFT` live; a small gain trim so it is not quieter than before (measure the same loop with HRTF on and off, 0.5 s of the beach at one spot); log which HRTF the device got (`ALC_HRTF_STATUS_SOFT`). Stereo sounds and UI sounds are not placed. Tests: `run-viewer-openal-log.bat` (log of the default), `run-viewer-openal-nohrtf.bat` (off through `alsoft-nohrtf.ini`).

**Status 2026-11-02 (stage 0 done, Adam listening on the beach):** OpenAL Soft 1.24.2 starts and plays (waves, gulls, wind, voice fine, output device switch works). Without HRTF it already sounds better than FMOD to Adam; with HRTF (the automatic headphone default) it sounds more realistic but is quieter and noticeably different; he may get used to it. Decision: HRTF stays an option, default off (`FSAudioHRTF`, checkbox in Preferences > Sound, written, not yet built). Temporary switch `LL_AUDIO_BACKEND=openal` (llstartup.cpp), launchers `run-viewer-openal.bat`, `-log`, `-nohrtf`.

**7. Remove FMOD (only after Adam has run OpenAL for a while).**
- `llaudioengine_fmodstudio.*`, `lllistener_fmodstudio.*`, `llstreamingaudio_fmodstudio.*`, `lllistener_ds3d.h` if unused; `FMODSTUDIO.cmake` and the include sites; `--fmodstudio` manifest arguments (`newview/CMakeLists.txt`, `viewer_manifest.py`, `fs_viewer_manifest.py`); `llstartup.cpp`, `llprogressview.cpp` (`LL_FMODSTUDIO` blocks), `llaudioengine.cpp` comment.
- Settings `FMODProfilerEnable`, `FMODResampleMethod`, `LL_BAD_FMODSTUDIO_DRIVER` in `client-readme.txt`; the About floater FMOD line; the FMOD credit and logo on the start page (`branding/rekindled/start`, uploaded copy on sl-op.com); FMOD license text in the installer; `fmod.dll`.
- Keep a git tag on the last FMOD commit.

## Risks

- OpenAL here is not exercised on Windows in this fork; expect a few small bugs in the shared engine paths (buffer eviction, channel steals, loop detection by sample offset, which the base comment calls "not reliable").
- Wind quality under main-thread stalls (stage 3 fixes it by moving generation off main).
- Streaming metadata and long-stream stability on the plugin path.
- Sounds may be a little different in level and falloff; Adam judges by ear.
- Linux/Mac: this plan makes OpenAL the only backend, so those builds get the same code; Mac uses its own CoreAudio route inside OpenAL Soft. Not tested here.

## Measure

Same lap and spot for both backends (the old stage 0 timers): `idle: audio` mean and max per 600-frame window, load-burst window after a teleport, device-change stall, audio thread CPU, and the main thread's frame time. Targets as in the old plan: at most 0.5 ms a frame over any window including a sim entry, no audio frame over 2 ms, no device-change stall.

**Status 2026-11-02, later (stages 1 and 2 done):** ALUT dropped (stage 1). Stage 2 (reduced scope, the full command/snapshot split is dropped, it saves ~0.1 ms): an audio thread (`LL::ThreadPool "OpenALAudio"`) owns the device reopen, the HRTF reset, the device enumeration and the shutdown ordering; the main thread only posts work and reads a cached device list. Baseline `idle: audio` was equal to FMOD (0.077 ms FMOD, 0.100 ms OpenAL). Adam's test: reopens 16-43 ms to a normal device, 0.8-1.1 s to the Oculus virtual device, none of it on the main thread, no AudioStall warnings.
Findings: (1) the device selector and HRTF affect only OpenAL sounds; the stream (VLC plugin) and voice (WebRTC) play on the Windows default device, so with FMOD the selector also moved the stream and now it does not (open: give the VLC plugin the device name). (2) OpenAL Soft 1.24.2's WASAPI backend builds its device list once and ignores `OnDeviceStateChanged`, so a Bluetooth headset that was off at startup is never listed (no added event, polling cannot help); the default-changed event works, so Default follows the headset. Patch proposed as a separate task (standalone test in `<work>\openal-enumtest`). (3) The loading screen and the start page show the OpenAL logo (FMOD logo files stay until stage 7; the FMOD licence credit is still needed while FMOD ships).
