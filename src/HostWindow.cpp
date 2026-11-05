#include "HostWindow.hpp"

#include <cstdio>
#include <cstring>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

// The client side of Wayland only (this talks to the host), so not wlr.hpp.
extern "C" {
#include <wlr/backend/wayland.h>
}

namespace raum {

namespace {

void onGlobal(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t version) {
    static_cast<HostWindow*>(data)->global(reg, name, iface, version);
}
void onGlobalRemove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kRegistry = {onGlobal, onGlobalRemove};

void onPing(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
const xdg_wm_base_listener kWmBase = {onPing};

void onSurfaceConfigure(void* data, xdg_surface*, uint32_t serial) {
    static_cast<HostWindow*>(data)->surfaceConfigure(serial);
}
const xdg_surface_listener kXdgSurface = {onSurfaceConfigure};

void onToplevelConfigure(void* data, xdg_toplevel*, int32_t w, int32_t h, wl_array* states) {
    bool fs = false;
    const auto* s = static_cast<const uint32_t*>(states->data);
    for (std::size_t i = 0; i < states->size / sizeof(uint32_t); ++i)
        if (s[i] == XDG_TOPLEVEL_STATE_FULLSCREEN) fs = true;
    static_cast<HostWindow*>(data)->configured(w, h, fs);
}
void onToplevelClose(void* data, xdg_toplevel*) { static_cast<HostWindow*>(data)->closeRequested(); }
void onToplevelBounds(void*, xdg_toplevel*, int32_t, int32_t) {}
void onToplevelCaps(void*, xdg_toplevel*, wl_array*) {}
const xdg_toplevel_listener kToplevel = {onToplevelConfigure, onToplevelClose, onToplevelBounds,
                                         onToplevelCaps};

void toDefaultQueue(void* proxy) {
    if (proxy) wl_proxy_set_queue(static_cast<wl_proxy*>(proxy), nullptr);
}

} // namespace

void HostWindow::global(wl_registry* reg, unsigned name, const char* iface, unsigned version) {
    if (std::strcmp(iface, wl_compositor_interface.name) == 0 && !m_compositor)
        m_compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4));
    else if (std::strcmp(iface, xdg_wm_base_interface.name) == 0 && !m_wmBase)
        m_wmBase = static_cast<xdg_wm_base*>(
            wl_registry_bind(reg, name, &xdg_wm_base_interface, version < 5 ? version : 5));
}

HostWindow::HostWindow(wlr_backend* wl, bool fullscreen, std::function<void(int, int)> onResize,
                       std::function<void()> onClose)
    : m_onResize(std::move(onResize)), m_onClose(std::move(onClose)) {
    m_remote = wlr_wl_backend_get_remote_display(wl);
    if (!m_remote) return;
    // Set up on a queue of its own, so the roundtrips here cannot run the
    // backend's handlers; afterwards everything goes back to the default
    // queue, which the backend dispatches.
    wl_event_queue* queue = wl_display_create_queue(m_remote);
    m_registry = wl_display_get_registry(m_remote);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_registry), queue);
    wl_registry_add_listener(m_registry, &kRegistry, this);
    wl_display_roundtrip_queue(m_remote, queue);
    if (!m_compositor || !m_wmBase) {
        std::fprintf(stderr, "[raum] the host has no xdg-shell; wlroots makes the window\n");
        wl_event_queue_destroy(queue);
        return;
    }
    xdg_wm_base_add_listener(m_wmBase, &kWmBase, this);
    m_surface = wl_compositor_create_surface(m_compositor);
    m_xdgSurface = xdg_wm_base_get_xdg_surface(m_wmBase, m_surface);
    xdg_surface_add_listener(m_xdgSurface, &kXdgSurface, this);
    m_toplevel = xdg_surface_get_toplevel(m_xdgSurface);
    xdg_toplevel_add_listener(m_toplevel, &kToplevel, this);
    xdg_toplevel_set_title(m_toplevel, "raum");
    xdg_toplevel_set_app_id(m_toplevel, "raum");
    if (fullscreen) xdg_toplevel_set_fullscreen(m_toplevel, nullptr);
    wl_surface_commit(m_surface);
    for (int i = 0; i < 50 && !m_configured; ++i)
        if (wl_display_roundtrip_queue(m_remote, queue) < 0) break;
    for (void* p : {static_cast<void*>(m_registry), static_cast<void*>(m_compositor),
                    static_cast<void*>(m_wmBase), static_cast<void*>(m_surface),
                    static_cast<void*>(m_xdgSurface), static_cast<void*>(m_toplevel)})
        toDefaultQueue(p);
    wl_event_queue_destroy(queue);
    if (!m_configured) std::fprintf(stderr, "[raum] the host never configured raum's window\n");
}

void HostWindow::createOutput(wlr_backend* wl) {
    if (m_configured && !m_output) m_output = wlr_wl_output_create_from_surface(wl, m_surface);
}

HostWindow::~HostWindow() {
    // The surface belongs to the output now; the backend destroys it with it.
    if (m_toplevel) xdg_toplevel_destroy(m_toplevel);
    if (m_xdgSurface) xdg_surface_destroy(m_xdgSurface);
    if (m_wmBase) xdg_wm_base_destroy(m_wmBase);
    if (m_compositor) wl_compositor_destroy(m_compositor);
    if (m_registry) wl_registry_destroy(m_registry);
}

void HostWindow::configured(int width, int height, bool fullscreen) {
    m_pendingW = width;
    m_pendingH = height;
    m_pendingFullscreen = fullscreen;
}

void HostWindow::surfaceConfigure(unsigned serial) {
    xdg_surface_ack_configure(m_xdgSurface, serial);
    std::fprintf(stderr, "[raum] host window %dx%d%s\n", m_pendingW, m_pendingH,
                 m_pendingFullscreen ? " fullscreen" : "");
    const bool first = !m_configured;
    m_configured = true;
    m_fullscreen = m_pendingFullscreen;
    if (first || m_pendingW != m_width || m_pendingH != m_height) {
        m_width = m_pendingW;
        m_height = m_pendingH;
        // Before the output exists the size is read with initialWidth/Height.
        if (!first && m_onResize) m_onResize(m_width, m_height);
    }
}

void HostWindow::setFullscreen(bool on) {
    if (!m_toplevel) return;
    if (on) xdg_toplevel_set_fullscreen(m_toplevel, nullptr);
    else xdg_toplevel_unset_fullscreen(m_toplevel);
    wl_display_flush(m_remote);
}

} // namespace raum
