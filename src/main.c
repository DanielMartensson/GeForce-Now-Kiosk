#define _GNU_SOURCE

// GeForce-Now-Kiosk — minimal WPE WebKit kiosk for Nvidia GeForce Now.
//
// Pure C11.  Two mutually exclusive builds (no fallback at runtime):
//
//   cmake -DIMFN_TARGET=ON   →  DRM/KMS only (STM32MP257F, no X11, no GL)
//   cmake -DIMFN_TARGET=OFF  →  X11+EGL+GLES3 only (desktop DEV)
//
// Feature flags (set below based on TARGET):
//   HAS_DRM  — DRM/KMS page flip via GBM (TARGET build)
//   HAS_X11  — X11 window system with EGL+GLES3 blit (DEV build)
//
// Render path:
//   WebKit composites → wpebackend-fdo exports dmabuf-backed EGLImage
//   DRM: eglExportDMABUFImageMESA → dmabuf fd → GBM import → page flip
//   X11: EGLImage → glEGLImageTargetTexture2DOES → GL blit
//
// Quit: Ctrl+Q.

// ---------------------------------------------------------------------------
// Feature flags — set to 1 or 0 based on build target (exactly one backend).
// ---------------------------------------------------------------------------
#ifdef TARGET
#define HAS_DRM 1
#define HAS_X11  0
#else
#define HAS_DRM 0
#define HAS_X11  1
#endif

// ---------------------------------------------------------------------------
// Includes — only pull in what the build target requires.
// ---------------------------------------------------------------------------
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdbool.h>
#include <wpe/fdo-egl.h>
#include <wpe/fdo.h>
#include <wpe/webkit.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#if HAS_DRM
#if __has_include(<GBM/gbm.h>)
#include <GBM/gbm.h>
#else
#include <gbm.h>
#endif
#include <drm_fourcc.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#endif

#if HAS_X11
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#endif

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

#if HAS_DRM
typedef EGLBoolean (*PFN_eglExportDMABUFImageMESA)(EGLDisplay, EGLImageKHR,
                                                   int *fds, EGLint *strides,
                                                   EGLint *offsets);
#endif

#if HAS_X11
typedef void (*PFN_glEGLImageTargetTexture2DOES)(GLenum target,
                                                 GLeglImageOES image);
#endif

typedef struct {
#if HAS_DRM
    // --- GBM + DRM/KMS ---
    EGLDisplay     egl;
    struct gbm_device *gbm;
    int            drm_fd;
    uint32_t       crtc_id, connector_id, plane_id;
    uint32_t       width, height, format;
    uint32_t       prop_fb_id, prop_src_w, prop_src_h, prop_crtc_w, prop_crtc_h;
    struct gbm_bo *prev_bo;
    uint32_t       prev_fb_id;
    int            flip_pending;

    // --- Evdev input ---
    int      kbd_fd, mou_fd;
    uint32_t mods;
#else
    // --- X11 + EGL + GLES3 ---
    EGLDisplay  egl;
    Display    *x11_dpy;
    Window      x11_win;
    Atom        x11_wm_delete;
    EGLContext  egl_ctx;
    EGLSurface  egl_surface;
    uint32_t    width, height;
    GLuint      gl_tex, gl_prog;
    GLint       gl_u_tex;
    PFN_glEGLImageTargetTexture2DOES   gl_bind_image;
#endif

    // --- WPE (common) ---
    struct wpe_view_backend_exportable_fdo *exportable;
    struct wpe_view_backend               *backend;
    WebKitWebView                         *view;
    struct wpe_fdo_egl_exported_image *displayed;
    struct wpe_fdo_egl_exported_image *pending;
    struct wpe_fdo_egl_exported_image *retire;
    int frame_pending, running;

    // --- xkbcommon (common) ---
    struct xkb_context *xkb_ctx;
    struct xkb_keymap  *xkb_keymap;
    struct xkb_state   *xkb_state;
} App;

static App g;

// ---------------------------------------------------------------------------
// Keysym mapping (evdev → XKB)
// ---------------------------------------------------------------------------

#if HAS_DRM
static uint32_t evdev_to_xkb(uint16_t c)
{
    switch (c) {
    case KEY_ENTER:      return XKB_KEY_Return;
    case KEY_ESC:        return XKB_KEY_Escape;
    case KEY_BACKSPACE:  return XKB_KEY_BackSpace;
    case KEY_TAB:        return XKB_KEY_Tab;
    case KEY_DELETE:     return XKB_KEY_Delete;
    case KEY_HOME:       return XKB_KEY_Home;
    case KEY_END:        return XKB_KEY_End;
    case KEY_PAGEUP:     return XKB_KEY_Page_Up;
    case KEY_PAGEDOWN:   return XKB_KEY_Page_Down;
    case KEY_LEFT:       return XKB_KEY_Left;
    case KEY_RIGHT:      return XKB_KEY_Right;
    case KEY_UP:         return XKB_KEY_Up;
    case KEY_DOWN:       return XKB_KEY_Down;
    case KEY_SPACE:      return XKB_KEY_space;
    case KEY_LEFTCTRL:   case KEY_RIGHTCTRL:  return XKB_KEY_Control_L;
    case KEY_LEFTSHIFT:  case KEY_RIGHTSHIFT: return XKB_KEY_Shift_L;
    case KEY_LEFTALT:    case KEY_RIGHTALT:   return XKB_KEY_Alt_L;
    case KEY_LEFTMETA:   case KEY_RIGHTMETA:  return XKB_KEY_Super_L;
    case KEY_F1:  return XKB_KEY_F1;  case KEY_F2:  return XKB_KEY_F2;
    case KEY_F3:  return XKB_KEY_F3;  case KEY_F4:  return XKB_KEY_F4;
    case KEY_F5:  return XKB_KEY_F5;  case KEY_F6:  return XKB_KEY_F6;
    case KEY_F7:  return XKB_KEY_F7;  case KEY_F8:  return XKB_KEY_F8;
    case KEY_F9:  return XKB_KEY_F9;  case KEY_F10: return XKB_KEY_F10;
    case KEY_F11: return XKB_KEY_F11; case KEY_F12: return XKB_KEY_F12;
    default: break;
    }
    if (c >= KEY_A && c <= KEY_Z) return 'a' + (c - KEY_A);
    if (c >= KEY_1 && c <= KEY_9) return '1' + (c - KEY_1);
    if (c == KEY_0) return '0';
    return 0;
}

static int evdev_kb_sym(uint16_t code)
{
    uint32_t sym = evdev_to_xkb(code);
    if (!sym || !g.xkb_keymap) return sym;
    const xkb_keysym_t *s = NULL;
    for (uint32_t kc = 8; kc < 256; kc++) {
        int n = xkb_keymap_key_get_syms_by_level(g.xkb_keymap, kc, 0, 0, &s);
        if (n > 0 && s && s[0] == (xkb_keysym_t)sym) return (int)kc;
    }
    return (int)(code + 8);
}

static void evdev_mod_set(uint16_t code, int pressed)
{
    uint32_t f = 0;
    switch (code) {
    case KEY_LEFTCTRL:  case KEY_RIGHTCTRL:  f = 0x04; break;
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: f = 0x01; break;
    case KEY_LEFTALT:   case KEY_RIGHTALT:   f = 0x08; break;
    case KEY_LEFTMETA:  case KEY_RIGHTMETA:  f = 0x10; break;
    }
    if (f) { if (pressed) g.mods |= f; else g.mods &= ~f; }
}

// ---------------------------------------------------------------------------
// Evdev input — keyboard and mouse from /dev/input/event*
// ---------------------------------------------------------------------------

static void evdev_kbd_read(void)
{
    struct input_event ev;
    while (read(g.kbd_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type != EV_KEY) continue;
        evdev_mod_set(ev.code, ev.value);
        uint32_t sym = evdev_to_xkb(ev.code);
        if (!sym) continue;
        if (ev.value == 1 && sym == XKB_KEY_q && (g.mods & 0x04)) {
            g.running = 0;
            return;
        }
        struct wpe_input_keyboard_event w = {
            .time      = (uint32_t)(g_get_monotonic_time() / 1000),
            .key_code  = sym,
            .hardware_key_code = (uint32_t)evdev_kb_sym(ev.code),
            .pressed   = ev.value ? 1 : 0,
            .modifiers = g.mods,
        };
        wpe_view_backend_dispatch_keyboard_event(g.backend, &w);
    }
}

static void evdev_mou_read(void)
{
    static int mx, my;
    static uint32_t btn;
    struct input_event ev;
    while (read(g.mou_fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type == EV_REL) {
            if (ev.code == REL_X) mx += ev.value;
            if (ev.code == REL_Y) my += ev.value;
            if (mx < 0) mx = 0;
            if (mx > (int)g.width)  mx = (int)g.width;
            if (my < 0) my = 0;
            if (my > (int)g.height) my = (int)g.height;
            struct wpe_input_pointer_event p = {
                .type = wpe_input_pointer_event_type_motion,
                .time = (uint32_t)(g_get_monotonic_time() / 1000),
                .x = mx, .y = my, .state = btn,
            };
            wpe_view_backend_dispatch_pointer_event(g.backend, &p);
        } else if (ev.type == EV_KEY && ev.code <= BTN_RIGHT) {
            uint32_t b = 0;
            if (ev.code == BTN_LEFT)   b = 1;
            if (ev.code == BTN_RIGHT)  b = 2;
            if (ev.code == BTN_MIDDLE) b = 3;
            if (b) {
                if (ev.value) btn |= 1u << (19 + b);
                else         btn &= ~(1u << (19 + b));
                struct wpe_input_pointer_event p = {
                    .type = wpe_input_pointer_event_type_button,
                    .time = (uint32_t)(g_get_monotonic_time() / 1000),
                    .x = mx, .y = my, .button = b, .state = btn,
                };
                wpe_view_backend_dispatch_pointer_event(g.backend, &p);
            }
        } else if (ev.type == EV_REL && (ev.code == REL_WHEEL ||
                                         ev.code == REL_HWHEEL)) {
            struct wpe_input_axis_2d_event ax;
            memset(&ax, 0, sizeof ax);
            ax.base.type = (enum wpe_input_axis_event_type)(
                wpe_input_axis_event_type_mask_2d
                | wpe_input_axis_event_type_motion_smooth);
            ax.base.time = (uint32_t)(g_get_monotonic_time() / 1000);
            ax.base.x = mx; ax.base.y = my;
            ax.base.modifiers = g.mods;
            float step = ev.value > 0 ? 10.0f : -10.0f;
            if (ev.code == REL_WHEEL) ax.y_axis = step;
            else                     ax.x_axis = step;
            wpe_view_backend_dispatch_axis_event(g.backend, &ax.base);
        }
    }
}

static void evdev_init(void)
{
    g.kbd_fd = g.mou_fd = -1;
    // Two passes: prefer devices whose names clearly say keyboard/mouse,
    // then fall back to any device with the right capabilities.
    for (int pass = 0; pass < 2 && (g.kbd_fd < 0 || g.mou_fd < 0); pass++) {
        for (int i = 0; i < 16; i++) {
            int need_kbd = g.kbd_fd < 0;
            int need_mou = g.mou_fd < 0;
            if (!need_kbd && !need_mou) break;
            char path[32];
            snprintf(path, sizeof path, "/dev/input/event%d", i);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            unsigned long bits[1 + KEY_MAX / (sizeof(long) * 8)] = {};
            if (ioctl(fd, EVIOCGBIT(0, sizeof bits), bits) < 0) {
                close(fd); continue;
            }
            int has_key = bits[1 + EV_KEY  / 64] & (1UL << (EV_KEY  % 64));
            int has_rel = bits[1 + EV_REL  / 64] & (1UL << (EV_REL  % 64));
            int take_kbd = need_kbd && has_key;
            int take_mou = need_mou && has_rel;
            if (pass == 0) {
                char name[128] = "";
                ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
                int is_kbd = strcasestr(name, "keyboard") != NULL;
                int is_mou = strcasestr(name, "mouse") != NULL
                          || strcasestr(name, "touchpad") != NULL
                          || strcasestr(name, "track")   != NULL;
                take_kbd = need_kbd && has_key && is_kbd;
                take_mou = need_mou && has_rel && is_mou;
            }
            if (take_kbd) g.kbd_fd = fd;
            else if (take_mou) g.mou_fd = fd;
            else close(fd);
            if (g.kbd_fd >= 0 && g.mou_fd >= 0) break;
        }
    }
    fprintf(stderr, "evdev: kbd=%s  mou=%s\n",
            g.kbd_fd  >= 0 ? "ok" : "none",
            g.mou_fd  >= 0 ? "ok" : "none");
}
#endif // HAS_DRM

// ---------------------------------------------------------------------------
// Keysym mapping (X11 KeySym → XKB)
// ---------------------------------------------------------------------------

#if HAS_X11
static uint32_t x11_to_xkb(KeySym ks)
{
    switch (ks) {
    case XK_Return:      case XK_KP_Enter: return XKB_KEY_Return;
    case XK_Escape:                         return XKB_KEY_Escape;
    case XK_BackSpace:                      return XKB_KEY_BackSpace;
    case XK_Tab:                            return XKB_KEY_Tab;
    case XK_Delete:                         return XKB_KEY_Delete;
    case XK_Home:                           return XKB_KEY_Home;
    case XK_End:                            return XKB_KEY_End;
    case XK_Page_Up:                        return XKB_KEY_Page_Up;
    case XK_Page_Down:                      return XKB_KEY_Page_Down;
    case XK_Left:                           return XKB_KEY_Left;
    case XK_Right:                          return XKB_KEY_Right;
    case XK_Up:                             return XKB_KEY_Up;
    case XK_Down:                           return XKB_KEY_Down;
    case XK_space:                          return XKB_KEY_space;
    default: break;
    }
    if (ks >= XK_F1 && ks <= XK_F12) return (uint32_t)(XKB_KEY_F1 + (ks - XK_F1));
    if (ks >= 0x21 && ks <= 0x7e)    return (uint32_t)ks;
    return 0;
}
#endif // HAS_X11

// ---------------------------------------------------------------------------
// WPE FDO export callbacks — called when WebKit has a new composited frame.
// ---------------------------------------------------------------------------

static void frame_wrong_size(struct wpe_fdo_egl_exported_image *image)
{
    wpe_view_backend_exportable_fdo_dispatch_frame_complete(g.exportable);
    wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(
        g.exportable, image);
}

#if HAS_DRM
static void on_egl_image(void *data, struct wpe_fdo_egl_exported_image *image)
{
    (void)data;

    if ((int)wpe_fdo_egl_exported_image_get_width(image)  != (int)g.width ||
        (int)wpe_fdo_egl_exported_image_get_height(image) != (int)g.height) {
        frame_wrong_size(image);
        return;
    }

    wpe_view_backend_exportable_fdo_dispatch_frame_complete(g.exportable);

    if (g.retire)
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(
            g.exportable, g.retire);
    g.retire  = g.displayed;
    g.displayed = NULL;
    if (g.pending)
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(
            g.exportable, g.pending);
    g.pending = image;

    // Export EGLImage → dmabuf fd → GBM bo → page flip.
    EGLImageKHR egl_img = wpe_fdo_egl_exported_image_get_egl_image(image);
    int fd = -1;
    EGLint stride = 0;
    PFN_eglExportDMABUFImageMESA export =
        (PFN_eglExportDMABUFImageMESA)eglGetProcAddress("eglExportDMABUFImageMESA");
    if (!export || !export(g.egl, egl_img, &fd, &stride, NULL) || fd < 0)
        goto failed;

    if (!g.flip_pending) {
        struct gbm_bo *bo = gbm_bo_import(
            g.gbm, GBM_BO_IMPORT_FD,
            &(struct gbm_import_fd_data){
                .fd     = fd,
                .width  = g.width,
                .height = g.height,
                .stride = (uint32_t)stride,
                .format = g.format,
            }, GBM_BO_USE_SCANOUT);
        if (bo) {
            uint32_t handle = gbm_bo_get_handle(bo).u32;
            uint32_t pitch  = gbm_bo_get_stride(bo);
            uint32_t fb_id  = 0;
            if (drmModeAddFB2(g.drm_fd, g.width, g.height, g.format,
                              &handle, &pitch, NULL, &fb_id, 0) == 0) {
                drmModeAtomicReq *req = drmModeAtomicAlloc();
                drmModeAtomicAddProperty(req, g.plane_id, g.prop_fb_id, fb_id);
                drmModeAtomicAddProperty(req, g.plane_id, g.prop_src_w,
                    ((uint64_t)g.width << 32) | g.height);
                drmModeAtomicAddProperty(req, g.plane_id, g.prop_src_h,
                    ((uint64_t)g.height << 32));
                drmModeAtomicAddProperty(req, g.plane_id, g.prop_crtc_w, g.width);
                drmModeAtomicAddProperty(req, g.plane_id, g.prop_crtc_h, g.height);
                if (drmModeAtomicCommit(g.drm_fd, req,
                                        DRM_MODE_ATOMIC_NONBLOCK, &g) == 0) {
                    g.prev_bo    = bo;
                    g.prev_fb_id = fb_id;
                    g.flip_pending = 1;
                    close(fd);
                    return;
                }
                drmModeRmFB(g.drm_fd, fb_id);
                drmModeAtomicFree(req);
            }
            gbm_bo_destroy(bo);
        }
    }
    g.displayed = image;
    g.pending   = NULL;

failed:
    if (fd >= 0) close(fd);
    g.frame_pending = 1;
}
#endif // HAS_DRM

#if HAS_X11
static void on_egl_image(void *data, struct wpe_fdo_egl_exported_image *image)
{
    (void)data;

    if ((int)wpe_fdo_egl_exported_image_get_width(image)  != (int)g.width ||
        (int)wpe_fdo_egl_exported_image_get_height(image) != (int)g.height) {
        frame_wrong_size(image);
        return;
    }

    wpe_view_backend_exportable_fdo_dispatch_frame_complete(g.exportable);

    if (g.retire)
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(
            g.exportable, g.retire);
    g.retire  = g.displayed;
    g.displayed = NULL;
    if (g.pending)
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(
            g.exportable, g.pending);
    g.pending = NULL;

    // Bind EGLImage as GL texture.
    EGLImageKHR egl_img = wpe_fdo_egl_exported_image_get_egl_image(image);
    if (!g.gl_tex) {
        glGenTextures(1, &g.gl_tex);
        glBindTexture(GL_TEXTURE_2D, g.gl_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, g.gl_tex);
    }
    g.gl_bind_image(GL_TEXTURE_2D, egl_img);

    g.displayed = image;
    g.frame_pending = 1;
}
#endif // HAS_X11

static void on_shm(void *data, struct wpe_fdo_shm_exported_buffer *buf)
{
    (void)data;
    wpe_view_backend_exportable_fdo_dispatch_frame_complete(g.exportable);
    wpe_view_backend_exportable_fdo_egl_dispatch_release_shm_exported_buffer(
        g.exportable, buf);
}

static const struct wpe_view_backend_exportable_fdo_egl_client export_client = {
#if HAS_DRM
    .export_fdo_egl_image = on_egl_image,
#endif
#if HAS_X11
    .export_fdo_egl_image = on_egl_image,
#endif
    .export_shm_buffer    = on_shm,
};

// ---------------------------------------------------------------------------
// WebKit init (shared)
// ---------------------------------------------------------------------------

static bool on_fullscreen(void *d, bool e) { (void)d; (void)e; return true; }

static WebKitWebView *on_create(WebKitWebView *v, WebKitNavigationAction *a, gpointer d)
{
    (void)v; (void)d;
    if (a) {
        WebKitURIRequest *r = webkit_navigation_action_get_request(a);
        if (r) {
            const char *u = webkit_uri_request_get_uri(r);
            if (u && *u) webkit_web_view_load_uri(g.view, u);
        }
    }
    return NULL;
}

static void init_webkit(void)
{
    wpe_loader_init("libWPEBackend-fdo-1.0.so");
    wpe_fdo_initialize_for_egl_display(g.egl);

    g.exportable = wpe_view_backend_exportable_fdo_egl_create(
        &export_client, &g, g.width, g.height);
    g.backend = wpe_view_backend_exportable_fdo_get_view_backend(g.exportable);

    WebKitWebViewBackend *wk = webkit_web_view_backend_new(
        g.backend,
        (GDestroyNotify)wpe_view_backend_exportable_fdo_destroy,
        g.exportable);
    g.view = webkit_web_view_new(wk);
    g_object_ref_sink(g.view);

    webkit_web_context_set_cache_model(
        webkit_web_context_get_default(), WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);

    WebKitNetworkSession *ns = webkit_web_view_get_network_session(g.view);
    if (!ns) ns = webkit_network_session_get_default();
    WebKitCookieManager *cm = webkit_network_session_get_cookie_manager(ns);
    webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
    webkit_network_session_set_itp_enabled(ns, FALSE);

    WebKitSettings *s = webkit_web_view_get_settings(g.view);
    webkit_settings_set_enable_webrtc(s, TRUE);
    webkit_settings_set_enable_media_stream(s, TRUE);
    webkit_settings_set_enable_mediasource(s, TRUE);
    webkit_settings_set_enable_encrypted_media(s, FALSE);
    webkit_settings_set_enable_write_console_messages_to_stdout(s, TRUE);

    const char *ua = getenv("IMFN_USER_AGENT");
    if (ua) webkit_settings_set_user_agent(s, ua);
    const char *hw = getenv("IMFN_MEDIA_HW_TYPES");
    if (hw) webkit_settings_set_media_content_types_requiring_hardware_support(s, hw);

    wpe_view_backend_add_activity_state(g.backend,
        wpe_view_activity_state_visible
        | wpe_view_activity_state_focused
        | wpe_view_activity_state_in_window);
    wpe_view_backend_set_fullscreen_handler(g.backend, on_fullscreen, NULL);
    g_signal_connect(g.view, "create", G_CALLBACK(on_create), NULL);

    const char *url = getenv("IMFN_URL");
    if (!url) url = "https://play.geforcenow.com/";
    webkit_web_view_load_uri(g.view, url);

    g.xkb_ctx    = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    g.xkb_keymap = xkb_keymap_new_from_names(g.xkb_ctx, NULL,
                                              XKB_KEYMAP_COMPILE_NO_FLAGS);
    g.xkb_state  = g.xkb_keymap ? xkb_state_new(g.xkb_keymap) : NULL;
}

// ---------------------------------------------------------------------------
// DRM/KMS backend (TARGET build)
// ---------------------------------------------------------------------------

#if HAS_DRM
static uint32_t drm_prop(uint32_t obj, uint32_t type, const char *name)
{
    drmModeObjectProperties *p = drmModeObjectGetProperties(g.drm_fd, obj, type);
    if (!p) return 0;
    for (uint32_t i = 0; i < p->count_props; i++) {
        drmModePropertyRes *pr = drmModeGetProperty(g.drm_fd, p->props[i]);
        if (pr) {
            int match = strcmp(pr->name, name) == 0;
            uint32_t id = pr->prop_id;
            drmModeFreeProperty(pr);
            if (match) { drmModeFreeObjectProperties(p); return id; }
        }
    }
    drmModeFreeObjectProperties(p);
    return 0;
}

static void on_page_flip(int fd, unsigned int frame, unsigned int sec,
                         unsigned int usec, void *data)
{
    (void)fd; (void)frame; (void)sec; (void)usec; (void)data;
    if (g.prev_fb_id) { drmModeRmFB(g.drm_fd, g.prev_fb_id); g.prev_fb_id = 0; }
    if (g.prev_bo)    { gbm_bo_destroy(g.prev_bo);    g.prev_bo = NULL; }
    g.flip_pending = 0;
}

static drmEventContext drm_ctx = {
    .version           = DRM_EVENT_CONTEXT_VERSION,
    .page_flip_handler = on_page_flip,
};

static int init_drm(void)
{
    drmDevicePtr devs[8];
    int ndev = drmGetDevices2(0, devs, 8);
    g.drm_fd = -1;
    for (int i = 0; i < ndev; i++) {
        if (!(devs[i]->available_nodes & DRM_NODE_PRIMARY)) continue;
        int fd = open(devs[i]->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmModeRes *r = drmModeGetResources(fd);
        if (r && r->count_crtcs > 0 && r->count_connectors > 0) {
            drmModeFreeResources(r);
            g.drm_fd = fd;
            break;
        }
        if (r) drmModeFreeResources(r);
        close(fd);
    }
    drmFreeDevices(devs, ndev);
    if (g.drm_fd < 0) { fprintf(stderr, "DRM: no device\n"); return -1; }

    drmModeRes *res = drmModeGetResources(g.drm_fd);
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(g.drm_fd, res->connectors[i]);
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            conn = c; break;
        }
        drmModeFreeConnector(c);
    }
    if (!conn) { drmModeFreeResources(res); return -1; }

    g.connector_id = conn->connector_id;
    drmModeModeInfo mode = conn->modes[0];
    g.width  = mode.hdisplay;
    g.height = mode.vdisplay;
    g.format = DRM_FORMAT_ARGB8888;

    drmModeEncoder *enc = drmModeGetEncoder(g.drm_fd, conn->encoders[0]);
    g.crtc_id = enc->crtc_id;
    drmModeFreeEncoder(enc);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    g.gbm = gbm_create_device(g.drm_fd);
    if (!g.gbm) return -1;

    g.egl = eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, g.gbm, NULL);
    if (g.egl == EGL_NO_DISPLAY) return -1;
    EGLint major, minor;
    if (!eglInitialize(g.egl, &major, &minor)) return -1;

    drmModePlaneRes *planes = drmModeGetPlaneResources(g.drm_fd);
    if (!planes) return -1;
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(g.drm_fd, planes->planes[i]);
        if (!p) continue;
        uint32_t crtc_idx = 0;
        for (int j = 0; j < res->count_crtcs; j++)
            if (res->crtcs[j] == g.crtc_id) { crtc_idx = j; break; }
        if (!(p->possible_crtcs & (1u << crtc_idx))) { drmModeFreePlane(p); continue; }
        int ok = 0;
        for (uint32_t f = 0; f < p->count_formats; f++)
            if (p->formats[f] == g.format) { ok = 1; break; }
        if (!ok) { drmModeFreePlane(p); continue; }

        g.plane_id = p->plane_id;
        g.prop_fb_id  = drm_prop(g.plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
        g.prop_src_w  = drm_prop(g.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
        g.prop_src_h  = drm_prop(g.plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
        g.prop_crtc_w = drm_prop(g.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
        g.prop_crtc_h = drm_prop(g.plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
        drmModeFreePlane(p);
        break;
    }
    drmModeFreePlaneResources(planes);
    if (!g.plane_id || !g.prop_fb_id) { fprintf(stderr, "DRM: no plane\n"); return -1; }

    if (drmSetMaster(g.drm_fd)) {
        fprintf(stderr, "DRM: need root or seat for drmSetMaster\n");
        return -1;
    }
    drmModeSetCrtc(g.drm_fd, g.crtc_id, 0, 0, 0, &g.connector_id, 1, &mode);
    fprintf(stderr, "DRM: %ux%u connector=%u crtc=%u plane=%u\n",
            g.width, g.height, g.connector_id, g.crtc_id, g.plane_id);
    return 0;
}
#endif // HAS_DRM

// ---------------------------------------------------------------------------
// X11 + EGL + GLES3 backend (DEV build)
// ---------------------------------------------------------------------------

#if HAS_X11
static int init_x11(void)
{
    g.x11_dpy = XOpenDisplay(NULL);
    if (!g.x11_dpy) { fprintf(stderr, "X11: no display\n"); return -1; }
    int screen = DefaultScreen(g.x11_dpy);
    g.width  = DisplayWidth(g.x11_dpy, screen);
    g.height = DisplayHeight(g.x11_dpy, screen);

    Window root = RootWindow(g.x11_dpy, screen);
    Visual *vis = DefaultVisual(g.x11_dpy, screen);
    Colormap cm = XCreateColormap(g.x11_dpy, root, vis, AllocNone);

    XSetWindowAttributes wa = {
        .colormap         = cm,
        .background_pixel = BlackPixel(g.x11_dpy, screen),
        .event_mask       = ExposureMask | KeyPressMask | KeyReleaseMask
                          | ButtonPressMask | ButtonReleaseMask
                          | PointerMotionMask | StructureNotifyMask,
        .override_redirect = True,
    };
    g.x11_win = XCreateWindow(g.x11_dpy, root, 0, 0, g.width, g.height, 0,
                              DefaultDepth(g.x11_dpy, screen), InputOutput,
                              vis, CWColormap | CWBackPixel
                              | CWEventMask | CWOverrideRedirect, &wa);
    if (!g.x11_win) return -1;
    g.x11_wm_delete = XInternAtom(g.x11_dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g.x11_dpy, g.x11_win, &g.x11_wm_delete, 1);
    XMapWindow(g.x11_dpy, g.x11_win);
    XFlush(g.x11_dpy);

    EGLint n;
    const EGLint cfg_attrs[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                 EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };

    g.egl = eglGetDisplay(g.x11_dpy);
    if (g.egl == EGL_NO_DISPLAY) return -1;
    EGLint major, minor;
    if (!eglInitialize(g.egl, &major, &minor)) return -1;

    EGLConfig cfg;
    if (!eglChooseConfig(g.egl, cfg_attrs, &cfg, 1, &n) || n == 0) return -1;
    g.egl_ctx = eglCreateContext(g.egl, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (!g.egl_ctx) return -1;
    g.egl_surface = eglCreateWindowSurface(g.egl, cfg,
                                           (EGLNativeWindowType)g.x11_win, NULL);
    if (!g.egl_surface) return -1;
    if (!eglMakeCurrent(g.egl, g.egl_surface, g.egl_surface, g.egl_ctx)) return -1;

    g.gl_bind_image = (PFN_glEGLImageTargetTexture2DOES)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!g.gl_bind_image) return -1;

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    static const char *vsrc =
        "#version 300 es\n"
        "out vec2 uv;\n"
        "void main(){\n"
        "  vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));\n"
        "  uv=vec2(p.x,1.0-p.y);\n"
        "  gl_Position=vec4(p*2.0-1.0,0,1);\n"
        "}\n";
    static const char *fsrc =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 uv;\n"
        "uniform sampler2D t;\n"
        "out vec4 c;\n"
        "void main(){c=texture(t,uv);}\n";
    glShaderSource(vs, 1, &vsrc, NULL); glCompileShader(vs);
    glShaderSource(fs, 1, &fsrc, NULL); glCompileShader(fs);
    g.gl_prog = glCreateProgram();
    glAttachShader(g.gl_prog, vs);
    glAttachShader(g.gl_prog, fs);
    glLinkProgram(g.gl_prog);
    glDeleteShader(vs); glDeleteShader(fs);
    g.gl_u_tex = glGetUniformLocation(g.gl_prog, "t");

    return 0;
}

static void present_x11(void)
{
    glViewport(0, 0, g.width, g.height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (g.gl_tex) {
        glUseProgram(g.gl_prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, g.gl_tex);
        glUniform1i(g.gl_u_tex, 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    eglSwapBuffers(g.egl, g.egl_surface);
    if (g.retire) {
        wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(
            g.exportable, g.retire);
        g.retire = NULL;
    }
}

static void x11_event(XEvent *ev)
{
    switch (ev->type) {
    case ClientMessage:
        if ((Atom)ev->xclient.data.l[0] == g.x11_wm_delete) g.running = 0;
        break;

    case KeyPress: case KeyRelease: {
        KeySym ks = XLookupKeysym((XKeyEvent *)&ev->xkey, 0);
        uint32_t sym = x11_to_xkb(ks);
        if (!sym) break;
        uint32_t keycode = 0;
        if (g.xkb_keymap) {
            const xkb_keysym_t *s = NULL;
            for (uint32_t kc = 8; kc < 256; kc++) {
                int n = xkb_keymap_key_get_syms_by_level(g.xkb_keymap, kc, 0, 0, &s);
                if (n > 0 && s && s[0] == (xkb_keysym_t)sym) { keycode = kc; break; }
            }
        }
        xkb_state_update_key(g.xkb_state, keycode ? keycode : ev->xkey.keycode,
                             ev->type == KeyPress ? XKB_KEY_DOWN : XKB_KEY_UP);
        if (ev->type == KeyPress && ks == XK_w && (ev->xkey.state & ControlMask))
            g.running = 0;
        if (ev->type == KeyPress && ks == XK_q && (ev->xkey.state & ControlMask))
            g.running = 0;
        uint32_t m = 0;
        if (ev->xkey.state & ControlMask) m |= 0x04;
        if (ev->xkey.state & ShiftMask)   m |= 0x01;
        if (ev->xkey.state & Mod1Mask)    m |= 0x08;
        if (ev->xkey.state & Mod4Mask)    m |= 0x10;
        struct wpe_input_keyboard_event w = {
            .time = (uint32_t)(g_get_monotonic_time() / 1000),
            .key_code = sym,
            .hardware_key_code = keycode ? keycode : (uint32_t)ev->xkey.keycode,
            .pressed = ev->type == KeyPress ? 1 : 0, .modifiers = m,
        };
        wpe_view_backend_dispatch_keyboard_event(g.backend, &w);
        break;
    }

    case ButtonPress: case ButtonRelease: {
        if (ev->xbutton.button == 4 || ev->xbutton.button == 5) {
            struct wpe_input_axis_2d_event ax;
            memset(&ax, 0, sizeof ax);
            ax.base.type = (enum wpe_input_axis_event_type)(
                wpe_input_axis_event_type_mask_2d | wpe_input_axis_event_type_motion_smooth);
            ax.base.time = (uint32_t)(g_get_monotonic_time() / 1000);
            ax.base.x = ev->xbutton.x; ax.base.y = ev->xbutton.y;
            ax.y_axis = (ev->xbutton.button == 4) ? 10.0f : -10.0f;
            wpe_view_backend_dispatch_axis_event(g.backend, &ax.base);
            break;
        }
        static const uint32_t map[] = { 0, 1, 3, 2 };
        uint32_t b = ev->xbutton.button <= 3 ? map[ev->xbutton.button] : (uint32_t)ev->xbutton.button;
        uint32_t s = 0;
        if (ev->type == ButtonPress) { if (b==1) s|=1<<20; if (b==2) s|=1<<21; if (b==3) s|=1<<22; }
        struct wpe_input_pointer_event p = {
            .type = wpe_input_pointer_event_type_button,
            .time = (uint32_t)(g_get_monotonic_time() / 1000),
            .x = ev->xbutton.x, .y = ev->xbutton.y, .button = b, .state = s,
        };
        wpe_view_backend_dispatch_pointer_event(g.backend, &p);
        break;
    }

    case MotionNotify: {
        struct wpe_input_pointer_event p = {
            .type = wpe_input_pointer_event_type_motion,
            .time = (uint32_t)(g_get_monotonic_time() / 1000),
            .x = ev->xmotion.x, .y = ev->xmotion.y,
        };
        wpe_view_backend_dispatch_pointer_event(g.backend, &p);
        break;
    }

    case ConfigureNotify:
        if (ev->xconfigure.width != (int)g.width ||
            ev->xconfigure.height != (int)g.height) {
            g.width  = ev->xconfigure.width;
            g.height = ev->xconfigure.height;
            wpe_view_backend_dispatch_set_size(g.backend, g.width, g.height);
        }
        break;
    default: break;
    }
}
#endif // HAS_X11

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void on_signal(int s) { (void)s; g.running = 0; }

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    memset(&g, 0, sizeof g);
    g.running  = 1;
#if HAS_DRM
    g.drm_fd   = -1;
    g.kbd_fd   = -1;
    g.mou_fd   = -1;
#endif
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

#if HAS_DRM
    // Embedded: DRM/KMS only.
    if (init_drm() < 0) {
        fprintf(stderr, "error: DRM init failed\n");
        return 1;
    }
    evdev_init();
    fprintf(stderr, "backend: DRM/KMS %ux%u\n", g.width, g.height);
    init_webkit();

    while (g.running) {
        struct pollfd fds[3];
        nfds_t nfds = 0;
        if (g.drm_fd >= 0) { fds[nfds].fd = g.drm_fd;    fds[nfds].events = POLLIN; nfds++; }
        if (g.kbd_fd >= 0) { fds[nfds].fd = g.kbd_fd;    fds[nfds].events = POLLIN; nfds++; }
        if (g.mou_fd >= 0) { fds[nfds].fd = g.mou_fd;    fds[nfds].events = POLLIN; nfds++; }
        if (nfds == 0) { usleep(50000); continue; }
        poll(fds, nfds, 50);
        for (nfds_t i = 0; i < nfds; i++) {
            if (!(fds[i].revents & POLLIN)) continue;
            if (fds[i].fd == g.drm_fd) drmHandleEvent(g.drm_fd, &drm_ctx);
            if (fds[i].fd == g.kbd_fd) evdev_kbd_read();
            if (fds[i].fd == g.mou_fd) evdev_mou_read();
        }

        while (g_main_context_pending(NULL))
            g_main_context_iteration(NULL, FALSE);
    }
#else
    // Desktop: X11+EGL+GLES3 only.
    if (init_x11() < 0) {
        fprintf(stderr, "error: X11 init failed\n");
        return 1;
    }
    const char *r = (const char *)glGetString(GL_RENDERER);
    if (r) fprintf(stderr, "GL renderer: %s\n", r);
    fprintf(stderr, "backend: X11+EGL %ux%u\n", g.width, g.height);
    init_webkit();

    while (g.running) {
        while (XPending(g.x11_dpy)) {
            XEvent ev;
            XNextEvent(g.x11_dpy, &ev);
            x11_event(&ev);
        }

        while (g_main_context_pending(NULL))
            g_main_context_iteration(NULL, FALSE);

        if (g.frame_pending) {
            present_x11();
            g.frame_pending = 0;
        }
    }
#endif

    // --- Cleanup ---
    if (g.view) g_object_unref(g.view);
#if HAS_DRM
    if (g.prev_bo) gbm_bo_destroy(g.prev_bo);
    if (g.gbm)     gbm_device_destroy(g.gbm);
    if (g.drm_fd >= 0) {
        drmDropMaster(g.drm_fd);
        close(g.drm_fd);
    }
#endif
#if HAS_X11
    if (g.gl_tex) glDeleteTextures(1, &g.gl_tex);
    eglMakeCurrent(g.egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g.egl_surface) eglDestroySurface(g.egl, g.egl_surface);
    if (g.egl_ctx)     eglDestroyContext(g.egl, g.egl_ctx);
    if (g.x11_win)     XDestroyWindow(g.x11_dpy, g.x11_win);
    if (g.x11_dpy)     XCloseDisplay(g.x11_dpy);
#endif
    if (g.xkb_state)  xkb_state_unref(g.xkb_state);
    if (g.xkb_keymap) xkb_keymap_unref(g.xkb_keymap);
    if (g.xkb_ctx)    xkb_context_unref(g.xkb_ctx);

    return 0;
}