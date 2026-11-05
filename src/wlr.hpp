#pragma once

// Every wlroots header raum uses, in one place. wlroots is C and its headers are
// written for C: xwayland.h names a struct member `class`, which C++ reserves,
// so that header is read with the word renamed. Everything else is plain C
// behind extern "C".

#include <pixman.h>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <xcb/xcb.h>

extern "C" {
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/backend/wayland.h>
#include <wlr/backend/x11.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/dmabuf.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#define class class_
#include <wlr/xwayland.h>
#undef class
}

// A wl_listener that calls a member function. Each one lives inside the object
// it serves; `connect` hooks it to a signal, `disconnect` (or destruction)
// takes it off again, so a listener is never left on a signal after its owner
// is gone.
template <typename Owner>
struct Listener {
    wl_listener raw{};
    Owner* owner = nullptr;
    void (Owner::*fn)(void*) = nullptr;
    bool linked = false;

    Listener() { wl_list_init(&raw.link); }
    ~Listener() { disconnect(); }
    Listener(const Listener&)            = delete;
    Listener& operator=(const Listener&) = delete;

    void connect(wl_signal* sig, Owner* o, void (Owner::*f)(void*)) {
        disconnect();
        owner = o;
        fn = f;
        raw.notify = &Listener::thunk;
        wl_signal_add(sig, &raw);
        linked = true;
    }
    void disconnect() {
        if (linked) wl_list_remove(&raw.link);
        wl_list_init(&raw.link);
        linked = false;
    }

private:
    static void thunk(wl_listener* l, void* data) {
        // `raw` is the first member: the listener is where its wl_listener is.
        Listener* self = reinterpret_cast<Listener*>(l);
        (self->owner->*self->fn)(data);
    }
};
