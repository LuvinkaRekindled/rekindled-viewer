#!/bin/bash
#
# Rekindled Viewer: start the viewer with extra diagnostics for a problem report.
# Run './rekindled-debug' instead of './rekindled'.  It writes:
#   ~/.rekindled_x64/logs/Rekindled.log   the viewer's log (more detailed with the switches below)
#   ~/rekindled-debug-console.txt         everything the viewer printed to the console
#   ~/rekindled-debug-system.txt          the graphics and system facts a report needs
# Please send those three files (and the picture, if it looks wrong).

SCRIPTSRC=$(readlink -f "$0" || echo "$0")
RUN_PATH=$(dirname "${SCRIPTSRC}" || echo .)
cd "${RUN_PATH}" || exit 1

# the facts a report needs, read before the viewer starts
{
    echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "kernel: $(uname -a)"
    echo "distribution: $(grep PRETTY_NAME /etc/os-release 2>/dev/null)"
    echo "session: XDG_SESSION_TYPE=${XDG_SESSION_TYPE} WAYLAND_DISPLAY=${WAYLAND_DISPLAY} DISPLAY=${DISPLAY}"
    echo "viewer: $(cat build_data.json 2>/dev/null)"
    echo
    if command -v vulkaninfo > /dev/null 2>&1; then
        echo "--- vulkaninfo --summary"
        vulkaninfo --summary 2>&1 | head -60
    else
        echo "vulkaninfo is not installed (package vulkan-tools); install it and run 'vulkaninfo --summary' for the report"
    fi
    echo
    echo "--- libraries the viewer and its plugins cannot find"
    for f in bin/do-not-directly-run-rekindled-bin bin/llplugin/libmedia_plugin_cef.so; do
        [ -f "$f" ] && LD_LIBRARY_PATH="${RUN_PATH}/lib:${LD_LIBRARY_PATH}" ldd "$f" 2>&1 | grep -i "not found"
    done
} > "${HOME}/rekindled-debug-system.txt" 2>&1

# the viewer's own diagnostic switches (they only add log lines)
export RK_VK_DIAG=1
export RK_VK_PIPEDIAG=1
export VK_LOADER_DEBUG=error,warn

echo "Starting Rekindled with diagnostics. Console output goes to ~/rekindled-debug-console.txt"
./rekindled "$@" 2>&1 | tee "${HOME}/rekindled-debug-console.txt"
echo
echo "Done. Please send: ~/.rekindled_x64/logs/Rekindled.log, ~/rekindled-debug-console.txt and ~/rekindled-debug-system.txt"
