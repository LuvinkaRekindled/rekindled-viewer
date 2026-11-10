# Email to NVIDIA (draft, 2026-11-10)

For Adam to edit and send. Fill the [brackets]. Nothing here names you or gives contact details; add what you want NVIDIA to have. Attach or paste Sample 1 from `NVIDIA-MARKS-SAMPLE.md` (the About-box text) and, if you want a second pair of eyes on it, the user terms paragraph from `app_settings/rekindled-licenses.txt`.

Where to send: the licence (section 19) names `nvidia-rtx-license-questions@nvidia.com` for licence questions. The pre-release notification (supplement section 4) is a separate web form at https://developer.nvidia.com/sw-notification; its fields are listed at the end.

---

**To:** nvidia-rtx-license-questions@nvidia.com
**Subject:** DLSS SDK: free open-source Second Life viewer, sample for approval and one licence question

Hello,

I maintain Rekindled Viewer [project page: https://sl-op.com/rekindled], a free, open-source third-party viewer for Second Life (a fork of the Firestorm viewer), source on GitHub [repository URL when public]. It runs on Windows 10/11 (64-bit) with a Vulkan renderer. I am not selling it; there will be donation links (Patreon and in-world). I plan a short public beta in [month] and I would like to do it correctly under the NVIDIA RTX SDKs License (v. March 14, 2024).

**What we use.** DLSS Super Resolution, DLAA and Ray Reconstruction from the public DLSS SDK. The release will ship the SDK's unmodified `nvngx_dlss.dll` and `nvngx_dlssd.dll` (release builds) inside the installer, alongside the application, and the viewer links the SDK's `nvsdk_ngx_d.lib`. The features are enabled only on NVIDIA GPUs that support them. Released builds will contain no component that is not in the public SDK.

**1. Approval of our marks sample (licence 7.1(b), 7.2(c)).** The About box will carry this text on its Licenses tab (text only; no NVIDIA logo):

> This application uses NVIDIA DLSS (Super Resolution, DLAA and Ray Reconstruction) from the NVIDIA RTX SDKs, used under the NVIDIA RTX SDKs License Agreement. NVIDIA, NVIDIA RTX, NVIDIA GeForce RTX and DLSS are trademarks and/or registered trademarks of NVIDIA Corporation in the United States and other countries. Rekindled is not sponsored or endorsed by NVIDIA.

Below it we give users terms for the two DLL files that follow the licence (rights stay with NVIDIA; use only as part of the application; no reverse engineering, notice removal, stand-alone distribution, modification or circumvention; no critical-system use; as-is, no liability) [attach the paragraph]. Please confirm in writing that this text is approved, or tell us what to change. Two questions about it: (a) is text-only attribution enough, or must the NVIDIA logo appear? (b) the application has a start-up splash window with a single status line; does 7.1(b) require the marks there too, or is the About box enough? If a splash line is needed, our proposal is: "Uses NVIDIA DLSS. NVIDIA and DLSS are trademarks of NVIDIA Corporation."

In our website, Patreon page and release post we plan to avoid NVIDIA's marks and say only "upscaling" and "modern graphics cards". Please tell us if you would rather we word that differently.

**2. A licence question (4(e)).** The viewer is licensed under the GNU LGPL 2.1 and is open source. The SDK is linked in two ways: the NGX static library (`nvsdk_ngx_d.lib`) is linked into the viewer executable at build time, and the two DLLs load at run time. Our repository contains none of the SDK (no headers, libraries or DLLs); people who build from source download the SDK themselves from NVIDIA's public GitHub repository and point the build at it. Release binaries ship the DLLs as object code only, unmodified. Does this arrangement comply with section 4(e), that the SDK may not be used in a way that makes it subject to an open source licence? If it does not, what would you want changed (for example loading the DLLs only at run time with our own thin loader, or a different arrangement)?

**3. "Commercial release" (supplement section 4).** The notification is required "prior to commercial release". Because the viewer is free and only donation-supported, please say whether you consider it a commercial release. I will submit the notification form in any case, with the details below.

Thank you. A reply to this address is fine.

[Your name / handle]
[Contact address]

---

## Fields for the notification form (https://developer.nvidia.com/sw-notification)

- Company name: [none / project name, or your name as the individual]
- Publisher and developer name: [Rekindled Viewer project / your name]
- NVIDIA SDK used: DLSS SDK (Super Resolution, DLAA, Ray Reconstruction). Version: [the SDK version in <work>\DLSS\DLSS; check its `include` or release notes]
- Application name: Rekindled Viewer
- Platform: PC (Windows)
- Scheduled ship date: [beta date]
- Web link to product: [https://sl-op.com/rekindled]

## Before sending

- Check the version of the SDK you ship and say it in the email and the form.
- Decide what name goes on the licence: you as an individual or the project.
- The user-terms paragraph and the 4(e) answer are exactly what a lawyer would want to see; if you know one, have them read both before the beta.
