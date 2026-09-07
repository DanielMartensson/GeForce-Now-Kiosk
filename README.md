# GeForce-Now-Kiosk

Minimal web client for [Nvidia GeForce Now](https://play.geforcenow.com/) built on **WPE WebKit + GStreamer WebRTC**.

No SDL, no ImGui, no Vulkan, no OpenGL ES (except the X11 DEV build) — pure C.
Designed for the absolute lowest memory and CPU footprint while delivering hardware-
accelerated cloud gaming, running fullscreen as a kiosk.

## Architecture

```
TARGET:  DRM/KMS (fullscreen, direct page flip)
DEV:     X11+EGL+GLES3 (fullscreen, GL blit)
   |  EGLImage → dmabuf / EGLImage→texture (zero-copy)
wpebackend-fdo 1.16.1          <- exports WebKit frames
   |  WPE bridge
WPE WebKit 2.52.6              <- WebRTC + media stream + H.264 decode
   |  webrtcbin
GStreamer 1.26.11               <- WebRTC + VAAPI/V4L2 HW decode
```

Two mutually exclusive build targets (no runtime fallback):

| Build                  | Backend                             |
|------------------------|-------------------------------------|
| `DEV` (default)        | X11+EGL+GLES3 only (desktop)        |
| `TARGET` (`-DIMFN_TARGET=ON`) | DRM/KMS only (STM32MP257F, no X11, no GL) |

## Quit

Press **Ctrl+Q** to quit.

## Dependencies built from source (deps/)

| Component        | Version | Why                                      |
|------------------|---------|------------------------------------------|
| libwpe           | 1.16.3  | WPE backend API                          |
| wpebackend-fdo   | 1.16.1  | dmabuf/EGLImage frame export             |
| GStreamer         | 1.26.11 | WebRTC + HW H.264 decode (patches needed)|
| WPE WebKit        | 2.52.6  | Browser engine (patches needed)          |

System packages (via apt): build-essential, cmake, ninja-build, meson,
pkg-config, curl, git, libglib2.0-dev, libsoup-3.0-dev, libepoxy-dev,
libegl-dev, libgles2-mesa-dev, libxkbcommon-dev, libdrm-dev, libffi-dev,
libxml2-dev, libxslt1-dev, libsqlite3-dev, libharfbuzz-dev,
libfreetype-dev, libfontconfig1-dev, libicu-dev, libpng-dev, libjpeg-dev,
libwebp-dev, libtasn1-dev, libpsl-dev, libseccomp-dev,
bubblewrap, xdg-dbus-proxy, libva-dev, i965-va-driver.

## Quick start (dev PC)

```bash
./setup.sh                      # build everything into deps/
./run.sh                        # opens play.geforcenow.com
```

## Building for the target (STM32MP257F)

```bash
cmake -B build -DIMFN_TARGET=ON   # DRM/KMS only, no X11, no GL
cmake --build build
```

## Scripts

| Script    | Purpose                                              |
|-----------|------------------------------------------------------|
| setup.sh  | Build all deps + app from source into deps/          |
| run.sh    | Launch the app with the correct LD_LIBRARY_PATH      |

## Runtime options (environment variables)

| Variable               | Meaning                                             |
|------------------------|-----------------------------------------------------|
| `IMFN_URL`             | Override the default URL (default: play.geforcenow.com) |
| `IMFN_USER_AGENT`      | Override the browser user agent string               |
| `IMFN_VIDEO_DECODER`   | Force a GStreamer HW decoder to MAX rank (e.g. `vah264dec`) |
| `IMFN_MEDIA_HW_TYPES`  | Force media content types to HW decode (e.g. `video/mp4; codecs="avc1"`) |
| `IMFN_PREFIX`          | Override bundled prefix (default: deps/install)      |

## Patches applied (via setup.sh)

| Patch | Purpose |
|-------|---------|
| `wpe-webkit-bwrap-unshare-net-webrtc.patch` | Let WebRTC see the host network through bubblewrap sandbox |
| `wpe-webkit-empty-body-js-mime.patch` | Fix empty-body JS MIME type (NVIDIA login CDN) |
| `wpe-webkit-documentloader-eventloop-include.patch` | Include EventLoop.h in DocumentLoader (unified-build fix) |
| `gstreamer-webrtcbin-audio-opus-ptmap-fallback.patch` | Opus fallback in webrtcbin PT mapping |
| `gstreamer-webrtcbin-balanced-to-maxbundle.patch` | Map BALANCED to MAX_BUNDLE for GFN SDP |

## Hardware decoders

| Platform                      | Decoder        |
|-------------------------------|----------------|
| x86 Intel (pre-Broadwell)    | `vah264dec` (i965 VA-API) |
| x86 Intel Broadwell+ / AMD   | `vah264dec` (iHD VA-API)  |
| STM32MP257F                   | `v4l2slh264dec` (SoC VPU) |

## License

MIT