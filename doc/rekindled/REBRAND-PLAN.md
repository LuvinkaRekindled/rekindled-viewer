# Rekindled Viewer: rebranding plan (2026-11-02)

The project (this fork of Phoenix Firestorm, branch `native-vk`) is renamed **Rekindled Viewer**. Brand direction (Adam): a blue and purple flame or spark, lettering to match. Hosting: a `rekindle` directory on sl-op.com with `start.html` as the viewer's start page once it is set up there (access and deploy flow: the `sl-op-hosting` note in the `D--SL` project memory and `D:\SL\README.md`; nothing is uploaded until Adam says so).

## 1. The marks (first set, in `branding/rekindled/`)

Hand built SVG, no fonts (the lettering is stroke paths with round caps, so it renders the same everywhere and has no font licence): `rekindled-mark.svg` (a blue to violet flame, an inner lighter flame, a four point spark at its heart), `rekindled-wordmark.svg` (lowercase "rekindled", the dot of the i is the spark), `rekindled-logo.svg` (mark + wordmark, horizontal), `rekindled-icon.svg` (the mark on a dark rounded square) and `rekindled-icon-small.svg` (the same without the inner flame, for 16-48 px), `rekindled.ico` (16, 24, 32, 48 from the simple art, 64, 128, 256 from the full art). `preview-sheet.png` shows them on light and dark. Colours: blue #1E6BFF, indigo #5B4DFF, violet #B03CFF, spark white to #8FC1FF, tile #101745 to #1D1359.

To do for the assets once the direction is agreed: transparent PNGs for the logo and wordmark on dark, a 1-bit/flat variant for the splash and the installer (NSIS header and welcome bitmaps), the `.icns` for macOS if it matters, a social/start page banner.

## 2. Where the name lives in the tree (survey)

- **Channel / name for the build:** `VIEWER_CHANNEL` (`indra/cmake/Variables.cmake`, the autobuild configuration `my_autobuild.xml`), shown in About, sent at login as the channel, and the base of the settings folder (`Roaming\Firestorm_x64`), the cache folder and the log path.
- **Executable and installer:** `firestorm-bin.exe` and `Firestorm_x64_...` names from `indra/newview/CMakeLists.txt` and `installers/windows/installer_template.nsi`; velopack prefix in `llvelopack.cpp`.
- **Icons:** `indra/newview/res/firestorm_icon.ico` / `.BMP` / `.png`, `res-sdl/firestorm_icon.BMP`, `icons/<channel>/` folders chosen by `ICON_PATH`, `install_icon.BMP`, the macOS `.icns`.
- **Strings and art in the UI:** `skins/default/xui/*/strings.xml` and floater files that say Firestorm (About, the Firestorm inventory folder, notifications), textures with the logo (login panel, "firestorm" in the toolbar/splash), `desktopnotifierlinux.cpp`, `llvoavatarself.cpp` (the client tag), `fsdata.cpp` (the support group, FS data service).
- **Start page:** the login panel loads `FSPanelLogin::loadLoginPage()`, which uses the grid's login page unless the setting `ForceLoginURL` is set: the start page is that URL (default value in `app_settings/settings.xml`), with the viewer's query parameters (channel, version, language, grid) that the page can read.
- **Things that depend on the channel string:** Firestorm-specific features gated on the name (the FS data service, support group chat, the update check, "FS" menu entries) and the viewer's registration with grids: to check before changing the channel.

## 3. Phases

1. **Marks** (this directory): agree the direction, then the final set and the raster/ICO/BMP exports.
2. **Display branding, behind one change set:** the name in the title bar, About, splash and login, the icon (exe resource, installer, taskbar), installer text and bitmaps, strings and logo textures; the executable, the settings and cache folders and the channel stay as they are for now, so nothing breaks: settings, cache, logs, every launcher and lap script keep working.
3. **Start page:** `https://sl-op.com/rekindle/start.html` on the brand, set as `ForceLoginURL` (a login page, not a browser home: it gets the login query parameters and can show news, release notes, the changes of the build). Deploy after Adam's go.
4. **Deep rename (decide later, a separate day):** the executable, settings / cache / log folders (with a one time migration from `Firestorm_x64`), the channel string, the installer file names, the update check, the CMake project names. Needs the channel decision below and a migration so no one loses settings.
5. **Licence and attribution:** the viewer stays GPL/LGPL with its headers; About and the installer keep the credit to Linden Lab and the Phoenix Firestorm project; the Firestorm name and artwork are theirs and are removed from our builds (the trademark rule of the Phoenix project for modified builds).

## 4. Decisions to make (recommendations)

1. **Direction of the marks** (colours, flame shape, the spark, the lettering): Adam to look at `preview-sheet.png`.
2. **Depth of the first rebrand:** display only (recommended), or the deep rename at once.
3. **Channel name:** `Rekindled` (a clean break; the grid sees a new viewer; features gated on "Firestorm" need a look) or keep the Firestorm channel string until the deep rename.
4. **Start page address:** `sl-op.com/rekindle/start.html` as said; whether it is a plain static page or reads a small JSON for news and release notes.
