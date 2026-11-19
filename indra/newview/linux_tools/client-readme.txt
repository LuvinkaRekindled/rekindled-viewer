Rekindled Viewer - Linux Beta README
-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=

Rekindled is a viewer for Second Life and OpenSim worlds, built on the
Firestorm viewer.  It draws with Vulkan only; there is no OpenGL.

1. Introduction
2. System requirements
3. Installing and running
4. Known issues
5. Troubleshooting
6. Reporting problems


1. INTRODUCTION
-=-=-=-=-=-=-=-

This is a BETA release for Linux.  Be aware of what that means here: the
Linux build has been compiled and has started up on a software Vulkan
device (Mesa llvmpipe, under WSL), but it has NOT been tested on a real
graphics card or a real Linux desktop.  Expect problems.  The tested
platform of this viewer is Windows.

Be aware that although this is a beta, the viewer connects to the real
Second Life world and changes you make there are permanent.


2. SYSTEM REQUIREMENTS
-=-=-=-=-=-=-=-=-=-=-=

    * A 64-bit Linux system with a desktop session (X11 or Wayland).
    * A graphics card and driver with Vulkan 1.3, including descriptor
      indexing (bindless sampled images, at least 1024 slots), multi-draw
      indirect, indirect first instance and buffer device addresses.  A
      card without them gets a plain message at start-up; there is no
      fallback path.  Run 'vulkaninfo' to see what your driver offers.
    * PulseAudio (or PipeWire's PulseAudio support) for sound.
    * An internet connection.

Ray traced shadows and reflections need a card with ray query support;
they are off by default.  There is no FSR or DLSS upscaling in the Linux
build yet, and no in-world media stream plugin.


3. INSTALLING AND RUNNING
-=-=-=-=-=-=-=-=-=-=-=-=-

The viewer runs entirely from the directory you unpacked it into; no
installation step is needed.  If you want a separate installation anyway,
run './install.sh'.  To add a menu entry for your desktop, see
REKINDLED_DESKTOPINSTALL.txt.

Run './rekindled' from the unpacked directory to start Rekindled.

User data (settings, cache, logs) is stored in the hidden directory
~/.rekindled_x64.  You can move it with the REKINDLED_X64_USER_DIR
environment variable.


4. KNOWN ISSUES
-=-=-=-=-=-=-=-

* Not run on a real Linux GPU yet; see the introduction.
* The shader and pipeline caches are empty on a first start, so the first
  start takes noticeably longer than later ones.
* The crash and hitch diagnostics of the Windows build do nothing on Linux.


5. TROUBLESHOOTING
-=-=-=-=-=-=-=-=-=

The viewer prints a lot of diagnostic information to the console it was
started from.  Most of it is also written to ~/.rekindled_x64/logs/Rekindled.log:
read that file first, especially the 'WARNING' and 'ERROR' lines.  The
start of the log says which graphics device the viewer picked and what it
found (lines marked 'Vulkan').

The viewer fails to start with a message about Vulkan:
   Your driver may be too old, or no Vulkan driver is installed.  Install
   your graphics vendor's current Vulkan driver (Mesa for AMD and Intel,
   the NVIDIA proprietary driver for NVIDIA) and check 'vulkaninfo'.

The viewer picks the wrong graphics card:
   On a machine with two cards, check the 'pickDevice' lines in the log.

The world is a flat colour, or something else looks wrong:
   Start the viewer with './rekindled-debug' instead of './rekindled'.  It
   adds diagnostic lines to the log and writes three files for a report
   (see REPORTING PROBLEMS).

A message that the media plugin failed:
   The embedded browser needs system libraries that are not in the package
   (for example libnspr4.so and libnss3.so; on Debian and Ubuntu install
   the packages libnspr4 and libnss3).  The viewer works without it, but
   web pages in-world and the built-in browser do not.

Sound problems:
   Rekindled uses OpenAL Soft.  See README-linux-voice.txt for voice.

Spacenavigator or joystick problems:
   See README-linux-joystick.txt.


6. REPORTING PROBLEMS
-=-=-=-=-=-=-=-=-=-=-

Please do not report problems with Rekindled to the Firestorm project.
Start the viewer with './rekindled-debug' and send these files from a run
that shows the problem:
   ~/.rekindled_x64/logs/Rekindled.log
   ~/rekindled-debug-console.txt
   ~/rekindled-debug-system.txt
and, if the picture looks wrong, a screenshot of the viewer window.
