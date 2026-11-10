# NVIDIA marks: sample for approval (draft, 2026-11-10)

Written for Adam to send to NVIDIA. The licence (`<work>\DLSS\DLSS\LICENSE.txt`, NVIDIA RTX SDKs LICENSE v. March 14, 2024, section 7) asks for this before the marks appear in a distributed build:

- 7.1(b): an application that incorporates the DLSS SDK must credit the SDK and include the NVIDIA Marks on splash screens, in the about box (if present) and in credits.
- 7.2(c): "submit a sample of each proposed use of NVIDIA's Marks at least two (2) weeks prior to the desired implementation of such use to obtain NVIDIA's prior written approval". No answer in ten business days = the sample is deemed unapproved.
- 7.1(a): naming NVIDIA as the supplier of the DLSS SDK on a website or collateral is subject to NVIDIA's prior review and written approval.
- The licence names no address for DLSS sample submissions (its one email address is for the RTX Video SDK). Find the channel on NVIDIA's developer site before sending; do not guess one.

## What the application is

Rekindled Viewer: a free third-party viewer for Second Life (a fork of Firestorm) with a Vulkan renderer. It uses the DLSS SDK for Super Resolution and DLAA and for Ray Reconstruction (the public SDK's `nvngx_dlss.dll` and `nvngx_dlssd.dll`). Released builds ship no DLSS component that is not in the public SDK. Distribution: free download; the first release is a beta.

## Sample 1: About box (Licenses tab)

Proposed text:

> This application uses NVIDIA DLSS (Super Resolution, DLAA and Ray Reconstruction) from the NVIDIA RTX SDKs, used under the NVIDIA RTX SDKs License Agreement. NVIDIA, NVIDIA RTX, NVIDIA GeForce RTX and DLSS are trademarks and/or registered trademarks of NVIDIA Corporation in the United States and other countries. Rekindled is not sponsored or endorsed by NVIDIA.

The same Licenses block carries terms for the two DLL files (nvngx_dlss.dll, nvngx_dlssd.dll) taken from sections 2c and 4 and 10-11 of the licence (rights stay with NVIDIA; use only as part of the application; no reverse engineering, notice removal, stand-alone distribution, modification or circumvention; no critical-system use; as-is, no liability).

Placement: Help menu > About, tab "Licenses", in the block "Rekindled additions", as the first line. Plain text in the viewer's normal about colour, no logo.

Question for NVIDIA: is text-only attribution enough, or should the NVIDIA logo appear? (The licence's grant in 7.2(c) lists NVIDIA RTX, NVIDIA GeForce RTX and NVIDIA Quadro RTX; the logo is not named.)

## Sample 2: start-up splash (optional, ask)

The viewer shows a Windows splash window with one line of status text while it starts. Proposed line, if NVIDIA wants the marks there:

> Uses NVIDIA DLSS. NVIDIA and DLSS are trademarks of NVIDIA Corporation.

Question for NVIDIA: does 7.1(b) require the marks on this splash, or is the About box enough for an application whose splash is a status line?

## Setting names in the Vulkan preferences panel

Today (Preferences > Graphics > Vulkan tab, `panel_preferences_graphics1.xml`): the checkbox "Upscale with DLSS / FSR (experimental)"; the combo "Upscaler" (Automatic (DLSS, else FSR) / NVIDIA DLSS / AMD FSR 3.1); the combo "Upscaling mode" (Performance (50%), Balanced (58%), Quality (67%), DLAA (100%, anti-aliasing only)); the checkbox "Ray reconstruction (denoises ray traced reflections)". NVIDIA's "RTX UI Developer Guidelines" ask for: a global DLSS toggle; the Super Resolution modes in the order Off, Auto, Quality, Balanced, Performance, Ultra-Performance, DLAA, with Auto first after Off; anti-aliasing and resolution scale disabled while DLSS Super Resolution is on; Ray Reconstruction as Off / Auto / On. Our panel differs from this in several places (no Auto and no Ultra-Performance mode, the modes run Performance to DLAA, ray reconstruction is a checkbox); these are about setting names and defaults, not about the marks, and are not part of the sample.

## Not for this request

The neural rendering option (nvngx_dlssnr.dll, labelled "Neural rendering (DLSS 5, ...)") is not part of the public SDK, is never packaged, and is not mentioned in any sample.
