# Features to promote at the first beta (draft, 2026-11-10)

Written for Adam, to cut down for the site and the release post. Each line says where the claim comes from, so nothing here is a promise the notes cannot back. "Measured" = a number in `GAINS.md` or the About page; "seen" = Adam saw it in the viewer; "built" = in the code, no number yet. Hardware for every measurement: GeForce RTX 5060 Ti, Windows 11.

## Lead with these

1. **Firestorm with a new engine.** Everything from Firestorm stays: its menus, RLV, the radar and the developer and debug tools (Developer menu, Render Metadata views, consoles, debug settings). Rekindled is not a new viewer to relearn. *(the About page's own first line)*
2. **Faster in crowds, measured.** Same crowded club, same window: stock Firestorm 7.2.4 about 66 fps, this viewer about 112 fps without upscaling (GAINS.md, October 7); November: about 96 fps with 72 avatars, ultra, DLSS on (About page). Say "your mileage depends on region and machine" as the page does.
3. **Vulkan only, no OpenGL.** The renderer talks to the card through Vulkan and nothing else. *(measured: runs about 228 fps in an empty scene, GAINS.md section 5)*
4. **Built for many cores.** A render thread records for the GPU; a shared worker pool does visibility, shadows, avatar animation and textures; audio has its own thread. *(About page; the crowd numbers above are the evidence)*
5. **Upscaling, your call.** DLSS (super resolution and DLAA) on NVIDIA, FSR 3.1 on any card. *(measured: DLSS Balanced 125+ fps in the crowd; FSR 3.1 tested on the RTX card)*

## Strong second tier

6. **Ray traced sun shadows** on cards with ray tracing hardware, as a setting, with soft edges (penumbra) and shadows for hair and alpha textures. *(seen; tested on the RTX card)*
7. **GPU work moved off the CPU:** bindless textures, culling and sorting on the GPU, texture animation on the GPU, GPU particles. *(texture animation: measured, 1.7 to 0.47 ms in GAINS.md; particles: built)*
8. **Quicker starts and loads:** shader and pipeline caches (pipeline build 160 to 13 ms a session), a decoded-texture disk cache with BC7 compression (one hub: 2412 MB of textures to 1616 MB), and faster texture arrival (fetch connection defaults 16/200, completion wake, first request at the size the screen implies). *(measured, project notes of 2026-10-29)*
9. **Better audio:** OpenAL instead of FMOD, optional HRTF, and Bluetooth headset voice fixed (the voice playout no longer drops to a call-quality stream). *(seen; Adam's headset)*
10. **Avatars in crowds:** hidden avatars update less often, rigged attachments of standing avatars update every 8th frame, skinning and shadows share work. *(measured, GAINS.md sections 4 and 5)*

## What the beta page should say honestly

- **Tested on one card.** Everything marked "works, tested" on the About page ran on a GeForce RTX 5060 Ti. AMD and Intel "should work, not tested yet"; ask for testers, as the page already does.
- **Not in the beta:** FSR 4 (planned), frame generation (left out on purpose), DLSS ray reconstruction for AMD (the vendor has no public Vulkan denoiser; see the notes of 2026-11-02).
- **Beta, not "stable".** It has been steady for you; say how it was tested (long sessions, crowds, the regression checklist) and ask for reports with the log attached.

## Check before the post goes up

- **DLSS (super resolution, DLAA and ray reconstruction) is NVIDIA's public SDK, and shipping its DLLs has conditions.** The licence (`<work>\DLSS\DLSS\LICENSE.txt`) lets an application distribute the SDK's runtime DLLs, if: users get terms at least as protective as the licence; the application credits the SDK and shows NVIDIA's marks where the licence says (credits, about or splash); the SDK is not used in a way that puts it under an open-source licence that requires source, derivative works or free redistribution (the viewer is LGPL: have someone who can read the licence check this one before the beta); and NVIDIA is told of known non-compliance. The About page and the installer text are where the credit goes.
- **DLSS-NR (the neural rendering option on the Vulkan tab) is not part of the beta's feature list.** Its DLL (`nvngx_dlssnr.dll`) is not in the public SDK and must never be packaged, until it is part of the official SDK. The option stays in the preferences, grayed out, and enables only when that DLL is beside the viewer (already coded: `LLPanelPreferenceGraphics::draw`, `gDeviceCaps.mHasNeuralRender`). Do not mention it on the site.
- **Credits and licence texts** go in a Licences tab of the About floater (NVIDIA's DLSS SDK attribution and marks, OpenAL Soft, AMD's FidelityFX SDK, and the rest that have no tab of their own); the About page on the site links the same names.
- **The numbers on the page** are dated and spot-specific. Re-run the club before the post if more than a few weeks pass, and keep the stock comparison dated October.
- **Feature switches still in the build** (`FSTextureFetchWake`, `FSTextureFetchGuessLevel`, `LL_VK_TEXONECOPY`) are on by default; the notes say to delete them once lived with. They are not user-facing and need no mention.
- **Not for the site:** any side-by-side with Aurora (you said so). One line is safe: Aurora is a new Rust codebase; Rekindled is Firestorm plus a new renderer.
