#!/usr/bin/env bash
#
# run.sh — launch GeForce-Now-Kiosk.
#
# On the dev PC, the bundled prefix (deps/install) holds WPE WebKit,
# libwpe, wpebackend-fdo and GStreamer 1.26 as private libraries that
# must NOT conflict with the system's versions.  This script exports
# the env vars so the binary and WebKit's helper processes
# (WPEWebProcess, WPENetworkProcess) all find the right libraries.
#
# Usage:
#   ./run.sh                       # opens play.geforcenow.com
#   ./run.sh https://other-url     # opens a different URL
#   IMFN_VIDEO_DECODER=vah264dec ./run.sh   # force a HW decoder
#
set -euo pipefail
cd "$(dirname "$0")"

: "${IMFN_PREFIX:=$PWD/deps/install}"
: "${IMFN_BUILD_DIR:=build}"

# ---------------------------------------------------------------------------
# Locate the binary
# ---------------------------------------------------------------------------
BINARY=""
if [ -x "$IMFN_BUILD_DIR/geforce-now-kiosk" ]; then
    BINARY="$IMFN_BUILD_DIR/geforce-now-kiosk"
elif command -v geforce-now-kiosk >/dev/null 2>&1; then
    BINARY="$(command -v geforce-now-kiosk)"
else
    echo "error: geforce-now-kiosk not found." >&2
    echo "       Run ./setup.sh first, or cmake -B build && cmake --build build" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# XDG_RUNTIME_DIR (needed by GLib for D-Bus / session bus)
# ---------------------------------------------------------------------------
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
    export XDG_RUNTIME_DIR="/run/user/$(id -u)"
fi

# ---------------------------------------------------------------------------
# Bundled prefix env — only on the dev PC (target has system install).
# ---------------------------------------------------------------------------
if [ -d "$IMFN_PREFIX/lib" ]; then
    export LD_LIBRARY_PATH="$IMFN_PREFIX/lib:$IMFN_PREFIX/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export GST_PLUGIN_PATH="$IMFN_PREFIX/lib/x86_64-linux-gnu/gstreamer-1.0"
    export GST_PLUGIN_SYSTEM_PATH=""
    export GST_PLUGIN_SCANNER="$IMFN_PREFIX/libexec/gstreamer-1.0/gst-plugin-scanner"
    export GST_REGISTRY="$IMFN_PREFIX/registry.bin"
fi

# ---------------------------------------------------------------------------
# WebKit bubblewrap sandbox: let the web process see the host network
# so WebRTC/ICE (GeForce Now) can reach ICE servers and peers.
#
# Two mechanisms:
#   1. The bwrap-unshare-net patch (patches/wpe-webkit-bwrap-unshare-net-webrtc.patch)
#      honours WEBKIT_ENABLE_NETWORK_ACCESS and skips --unshare-net for it.
#   2. Setting WEBKIT_INSPECTOR_SERVER also makes BubblewrapLauncher skip
#      --unshare-net (as a fallback even without the patch).
# ---------------------------------------------------------------------------
export WEBKIT_ENABLE_NETWORK_ACCESS=1
if [ -z "${WEBKIT_INSPECTOR_SERVER:-}" ]; then
    export WEBKIT_INSPECTOR_SERVER="127.0.0.1:0"
fi

# ---------------------------------------------------------------------------
# Optional: force a hardware video decoder to MAX rank.
#   x86 Intel/AMD:  IMFN_VIDEO_DECODER=vah264dec
#   STM32MP257F:    IMFN_VIDEO_DECODER=v4l2slh264dec
# ---------------------------------------------------------------------------
if [ -n "${IMFN_VIDEO_DECODER:-}" ]; then
    export GST_PLUGIN_FEATURE_RANK="${IMFN_VIDEO_DECODER}:MAX"
fi

exec "$BINARY" "$@"