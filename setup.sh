#!/usr/bin/env bash
#
# setup.sh — build GeForce-Now-Kiosk + every dependency from source into deps/.
#
#   Only the four libraries required for GeForce Now are built:
#     libwpe, wpebackend-fdo, GStreamer (1.26, with WebRTC), WPE WebKit
#   NO SDL3, NO ImGui, NO Vulkan.  Pure C, pure EGL/GLES3.
#
#   Everything lands under deps/ — never touches the system. Sources are
#   fetched once, builds are cached, so re-running is fast and idempotent.
#
# Build order (dependency graph):
#   libwpe -> wpebackend-fdo -> GStreamer 1.26 -> WPE WebKit -> GeForce-Now-Kiosk
#
# Usage:
#   ./setup.sh                          # build everything into deps/install
#   ./setup.sh --prefix ~/imfn          # install into a private prefix
#   ./setup.sh --jobs=4                 # parallel build
#   ./setup.sh --decoder=vah264dec      # bake in a default HW decoder
#
set -euo pipefail

PREFIX="${IMFN_PREFIX:-$PWD/deps/install}"
SRC_DIR="${IMFN_SRC_DIR:-$PWD/deps/src}"
JOBS="${IMFN_JOBS:-$(nproc)}"
EXTRA_CMAKE_ARGS=""

WITH_LIBWPE=1
WITH_WPEBACKEND=1
WITH_GSTREAMER=1
WITH_WPEWEBKIT=1
VDECODER=""

# --- read command line ---
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix=*)     PREFIX="${1#*=}" ;;
        --jobs=*)       JOBS="${1#*=}" ;;
        --decoder=*)    VDECODER="${1#*=}" ;;
        --skip-libwpe)  WITH_LIBWPE=0 ;;
        --skip-wpebackend-fdo) WITH_WPEBACKEND=0 ;;
        --skip-gstreamer) WITH_GSTREAMER=0 ;;
        --skip-wpewebkit) WITH_WPEWEBKIT=0 ;;
        -h|--help)
            sed -n '2,42p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) echo "unknown option: $1 (see --help)" >&2; exit 1 ;;
    esac
    shift
done

LIBDIR="lib"
[ "$(uname -m)" = "x86_64" ] && LIBDIR="lib/x86_64-linux-gnu"

# Environment so later packages find earlier ones.
export PKG_CONFIG_PATH="$PREFIX/$LIBDIR/pkgconfig:$PREFIX/lib/pkgconfig"
export CMAKE_PREFIX_PATH="$PREFIX"
export LD_LIBRARY_PATH="$PREFIX/$LIBDIR:$PREFIX/lib"
export PATH="$PREFIX/bin:$PATH"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
need_sudo() { [ "$(id -u)" -eq 0 ]; }

require_cmd() {
    for c in "$@"; do
        command -v "$c" >/dev/null 2>&1 || {
            echo "FATAL: missing tool '$c'. Install build tools first." >&2
            exit 1
        }
    done
}

fetch() {
    local url="$1" name="$2"
    local tar
    tar="$SRC_DIR/$(basename "$url")"
    mkdir -p "$SRC_DIR"
    if [ -d "$SRC_DIR/$name" ]; then
        echo "   [skip] $name already extracted"
        return
    fi
    if [ ! -f "$tar" ]; then
        echo "   [fetch] $url"
        curl -fSL --retry 3 -o "$tar" "$url"
    fi
    echo "   [extract] $name"
    mkdir -p "$SRC_DIR/$name.tmp"
    tar -xf "$tar" -C "$SRC_DIR/$name.tmp" --strip-components=1
    mv "$SRC_DIR/$name.tmp" "$SRC_DIR/$name"
}

fetch_gstreamer() {
    local ver="1.26.11"
    local dest="$SRC_DIR/gstreamer-$ver"
    [ -d "$dest" ] && { echo "   [skip] gstreamer-$ver already present"; return; }
    echo "   [clone] GStreamer monorepo @ $ver"
    git clone --depth 1 --branch "$ver" \
        https://gitlab.freedesktop.org/gstreamer/gstreamer.git "$dest"
}

apply_patch() {
    local srcdir="$1" patch="$2"
    case "$patch" in
        /*) ;;
        *)  patch="$PWD/$patch" ;;
    esac
    if [ ! -f "$patch" ]; then
        echo "   [warn] patch not found: $patch" >&2
        return 1
    fi
    if ( cd "$srcdir" && git apply --check "$patch" 2>/dev/null ); then
        echo "   [patch] $(basename "$patch")"
        ( cd "$srcdir" && git apply "$patch" )
    elif ( cd "$srcdir" && git apply --reverse --check "$patch" 2>/dev/null ); then
        echo "   [skip]  $(basename "$patch") already applied"
    else
        echo "   [FAIL]  $(basename "$patch") does not apply" >&2
        return 1
    fi
}

run_install() {
    local dir="$1"
    local k="$PREFIX"
    while [ "$k" != "/" ] && [ ! -w "$k" ]; do k=$(dirname "$k"); done
    if [ -w "$k" ]; then
        cmake --install "$dir"
    else
        echo "   [sudo] installing into $PREFIX"
        sudo env PKG_CONFIG_PATH="$PKG_CONFIG_PATH" \
            LD_LIBRARY_PATH="$LD_LIBRARY_PATH" PATH="$PATH" \
            cmake --install "$dir"
    fi
}

meson_install() {
    local dir="$1"
    local k="$PREFIX"
    while [ "$k" != "/" ] && [ ! -w "$k" ]; do k=$(dirname "$k"); done
    if [ -w "$k" ]; then
        meson install -C "$dir"
    else
        echo "   [sudo] installing into $PREFIX"
        sudo env LD_LIBRARY_PATH="$LD_LIBRARY_PATH" PATH="$PATH" meson install -C "$dir"
    fi
}

# ---------------------------------------------------------------------------
# 1. libwpe 1.16.3 — WPE backend API.  Tiny: ~50KB.
# ---------------------------------------------------------------------------
build_libwpe() {
    local d="$SRC_DIR/libwpe-1.16.3"
    [ -d "$d" ] || return 0
    say "libwpe 1.16.3"
    cmake -S "$d" -B "$d/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DBUILD_DOCS=OFF
    cmake --build "$d/build" -j"$JOBS"
    run_install "$d/build"
}

# ---------------------------------------------------------------------------
# 2. wpebackend-fdo 1.16.1 — exports WebKit frames as dmabuf/EGLImage.
#    Meson project.  No audio extension needed.
# ---------------------------------------------------------------------------
build_wpebackend() {
    local d="$SRC_DIR/wpebackend-fdo-1.16.1"
    [ -d "$d" ] || return 0
    say "wpebackend-fdo 1.16.1"
    meson setup "$d/build" "$d" \
        --prefix="$PREFIX" \
        --buildtype=release \
        -Dbuild_docs=false
    meson compile -C "$d/build" -j"$JOBS"
    meson_install "$d/build"
}

# ---------------------------------------------------------------------------
# 3. GStreamer 1.26.11 (monorepo) — media framework + WebRTC for GeForce Now.
#
#    Minimal subset:
#      base  — audioconvert, videoconvert, playbuffer, audioparse
#      good  — rtp* plugins (opus/vp8/h264 depay/pay), level, volume, audioparsers
#      bad   — webrtcbin, h264parse, va (vah264dec), dtls, srtp
#      libav — H.264 decode fallback (only used if no HW decoder is present)
#
#    Disabled: ugly, libde265, opener, vaapi (use va instead)
#    Two patches applied (see patches/): opus ptmap fallback + balanced->maxbundle
# ---------------------------------------------------------------------------
build_gstreamer() {
    local d="$SRC_DIR/gstreamer-1.26.11"
    [ -d "$d" ] || return 0
    say "GStreamer 1.26.11 (WebRTC + H.264 + hardware decode)"
    apply_patch "$d" "patches/gstreamer-webrtcbin-audio-opus-ptmap-fallback.patch"
    apply_patch "$d" "patches/gstreamer-webrtcbin-balanced-to-maxbundle.patch"

    meson setup "$d/build" "$d" \
        --prefix="$PREFIX" \
        --buildtype=release \
        -Dbase=enabled \
        -Dgood=enabled \
        -Dbad=enabled \
        -Dugly=disabled \
        -Dlibav=enabled \
        -Dwebrtc=enabled \
        -Dgst-plugins-base:examples=disabled \
        -Dgst-plugins-base:tests=disabled \
        -Dgst-plugins-good:tests=disabled \
        -Dgst-plugins-good:examples=disabled \
        -Dgst-plugins-bad:webrtcdsp=disabled \
        -Dgst-plugins-bad:va=enabled \
        -Dgst-plugins-bad:tests=disabled \
        -Dtests=disabled \
        -Dexamples=disabled \
        -Dbenchmarks=disabled \
        -Dgtk_doc=disabled
    meson compile -C "$d/build" -j"$JOBS"
    meson_install "$d/build"
}

# ---------------------------------------------------------------------------
# 4. WPE WebKit 2.52.6 — the browser engine, MAXIMALLY TRIMMED.
#
#    GeForce Now (play.geforcenow.com) is an Angular SPA that:
#      - opens an RTCPeerConnection for the game stream (WebRTC)
#      - uses getUserMedia for camera/mic access prompts (media stream)
#      - runs heavy JS (JIT is mandatory for performance)
#      - receives H.264 video + Opus audio via WebRTC
#
#    What's OFF (not used by the cloud-gaming SPA):
#      WebGL, WebGPU, accessibility, PDF, XSLT, spelling, geolocation,
#      gamepad, notifications, media recorder, media session, touch events,
#      fullscreen API, pointer lock, WebXR, content extensions/filters,
#      encrypted media, memory sampler, MHTML, offscreen canvas, etc.
#
#    What's ON:
#      media stream + WebRTC (mandatory), video (GStreamer), JIT (mandatory),
#      media source (WebRTC pipeline), web audio (Opus), bubblewrap sandbox
#      (security: the net-access patch lets WebRTC reach the network).
# ---------------------------------------------------------------------------
build_wpewebkit() {
    local d="$SRC_DIR/wpewebkit-2.52.6"
    [ -d "$d" ] || return 0
    say "WPE WebKit 2.52.6 (GeForce Now optimized)"
    apply_patch "$d" "patches/wpe-webkit-bwrap-unshare-net-webrtc.patch"
    apply_patch "$d" "patches/wpe-webkit-empty-body-js-mime.patch"

    cmake -S "$d" -B "$d/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DPORT=WPE \
        -DENABLE_MEDIA_STREAM=ON \
        -DENABLE_WEB_RTC=ON \
        -DENABLE_VIDEO=ON \
        -DENABLE_MEDIA_SOURCE=ON \
        -DENABLE_WEB_AUDIO=ON \
        -DENABLE_JIT=ON \
        -DENABLE_DFG_JIT=ON \
        -DENABLE_FTL_JIT=ON \
        -DENABLE_WEBGL=OFF \
        -DENABLE_WEBGPU=OFF \
        -DENABLE_ACCESSIBILITY=OFF \
        -DENABLE_WEBDRIVER=OFF \
        -DENABLE_PDFJS=OFF \
        -DENABLE_XSLT=OFF \
        -DENABLE_SPEECH_SYNTHESIS=OFF \
        -DENABLE_SPELLCHECK=OFF \
        -DENABLE_CONTENT_EXTENSIONS=OFF \
        -DENABLE_CONTENT_FILTERING=OFF \
        -DENABLE_WEB_AUTHN=OFF \
        -DENABLE_GAMEPAD=OFF \
        -DENABLE_DEVICE_ORIENTATION=OFF \
        -DENABLE_NOTIFICATIONS=OFF \
        -DENABLE_GEOLOCATION=OFF \
        -DENABLE_FULLSCREEN_API=OFF \
        -DENABLE_TOUCH_EVENTS=OFF \
        -DENABLE_WEBXR=OFF \
        -DENABLE_MEDIA_CAPTURE=OFF \
        -DENABLE_MEDIA_RECORDER=OFF \
        -DENABLE_MEDIA_SESSION=OFF \
        -DENABLE_ENCRYPTED_MEDIA=OFF \
        -DENABLE_MEMORY_SAMPLER=OFF \
        -DENABLE_MHTML=OFF \
        -DENABLE_DARK_MODE_CSS=OFF \
        -DENABLE_SAMPLING_PROFILER=OFF \
        -DENABLE_WEBASSEMBLY=OFF \
        -DENABLE_OFFSCREEN_CANVAS=OFF \
        -DENABLE_OFFSCREEN_CANVAS_IN_WORKERS=OFF \
        -DENABLE_REMOTE_INSPECTOR=OFF \
        -DENABLE_DEVELOPER_MODE=OFF \
        -DENABLE_API_TESTS=OFF \
        -DENABLE_MINIBROWSER=OFF \
        -DENABLE_COG=OFF \
        -DENABLE_INTROSPECTION=OFF \
        -DENABLE_JOURNALD_LOG=OFF \
        -DENABLE_BUBBLEWRAP_SANDBOX=ON \
        -DUSE_LIBBACKTRACE=OFF \
        -DUSE_AVIF=OFF \
        -DUSE_JPEGXL=OFF \
        -DUSE_LCMS=OFF \
        -DUSE_SKIA=OFF \
        -DUSE_SKIA_ENCODERS=OFF

    cmake --build "$d/build" -j"$JOBS"
    run_install "$d/build"
}

# ---------------------------------------------------------------------------
# 5. GeForce-Now-Kiosk — this project (pure C, DRM/KMS, ~20KB binary).
# ---------------------------------------------------------------------------
build_app() {
    local root="$PWD"
    say "GeForce-Now-Kiosk"
    cmake -S "$root" -B "$root/build" \
        -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_PREFIX_PATH="$PREFIX"
    cmake --build "$root/build" -j"$JOBS"
    cmake --install "$root/build"
}

# ---------------------------------------------------------------------------
# Full dependency list
# ---------------------------------------------------------------------------
declare -a DEPS=(
    "libwpe|$WITH_LIBWPE|https://wpewebkit.org/releases/libwpe-1.16.3.tar.xz|build_libwpe"
    "wpebackend-fdo|$WITH_WPEBACKEND|https://wpewebkit.org/releases/wpebackend-fdo-1.16.1.tar.xz|build_wpebackend"
    "GStreamer|$WITH_GSTREAMER|https://gitlab.freedesktop.org/gstreamer/gstreamer.git|build_gstreamer"
    "WPE-WebKit|$WITH_WPEWEBKIT|https://wpewebkit.org/releases/wpewebkit-2.52.6.tar.xz|build_wpewebkit"
)

name_for_fetch() {
    case "$1" in
        *libwpe-1.16.3*)       echo "libwpe-1.16.3" ;;
        *wpebackend-fdo-1.16.1*) echo "wpebackend-fdo-1.16.1" ;;
        *wpewebkit-2.52.6*)    echo "wpewebkit-2.52.6" ;;
    esac
}

main() {
    echo "GeForce-Now-Kiosk — from-source setup"
    echo "  prefix : $PREFIX"
    echo "  source : $SRC_DIR"
    echo "  jobs   : $JOBS"
    echo

    require_cmd cmake curl tar git ninja meson pkg-config

    for row in "${DEPS[@]}"; do
        IFS='|' read -r _name _en _url _fn <<<"$row"
        [ "$_en" = 1 ] || continue
        case "$_url" in
            *gstreamer.git*) fetch_gstreamer ;;
            *)               fetch "$_url" "$(name_for_fetch "$_url")" ;;
        esac
        "$_fn"
    done

    build_app

    say "Done.  Everything installed into: $PREFIX"
    echo "Run with:  ./run.sh"
}

main "$@"