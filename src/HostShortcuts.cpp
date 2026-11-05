#include "HostShortcuts.hpp"

#include <cstdio>
#include <cstring>

#include <wayland-client.h>

#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"

// Only the client side of Wayland here (this talks to the host), so not wlr.hpp.
extern "C" {
#include <wlr/backend/multi.h>
#include <wlr/backend/wayland.h>
}

namespace raum {

namespace {

void registryGlobal(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t version) {
    auto* self = static_cast<HostShortcuts*>(data);
    if (std::strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) == 0) {
        self->m_manager = static_cast<zwp_keyboard_shortcuts_inhibit_manager_v1*>(
            wl_registry_bind(reg, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1));
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0 && !self->m_seat) {
        self->m_seat = static_cast<wl_seat*>(
            wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5));
    }
}

void registryRemove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener kRegistry = {registryGlobal, registryRemove};
const zwp_keyboard_shortcuts_inhibitor_v1_listener kInhibitor = {&HostShortcuts::onActive,
                                                                  &HostShortcuts::onInactive};

void findWl(wlr_backend* b, void* data) {
    if (wlr_backend_is_wl(b)) *static_cast<wlr_backend**>(data) = b;
}

} // namespace

void HostShortcuts::onActive(void* data, zwp_keyboard_shortcuts_inhibitor_v1*) {
    static_cast<HostShortcuts*>(data)->m_active = true;
    std::fprintf(stderr, "[raum] the host passes its shortcuts through\n");
}

void HostShortcuts::onInactive(void* data, zwp_keyboard_shortcuts_inhibitor_v1*) {
    static_cast<HostShortcuts*>(data)->m_active = false;
}

HostShortcuts::HostShortcuts(wlr_backend* backend, wlr_output* output) {
    wlr_backend* wl = nullptr;
    if (wlr_backend_is_multi(backend)) wlr_multi_for_each_backend(backend, findWl, &wl);
    else if (wlr_backend_is_wl(backend)) wl = backend;
    if (!wl || !wlr_output_is_wl(output)) return;
    m_remote = wlr_wl_backend_get_remote_display(wl);
    if (!m_remote) return;
    m_queue = wl_display_create_queue(m_remote);
    m_registry = wl_display_get_registry(m_remote);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_registry), m_queue);
    wl_registry_add_listener(m_registry, &kRegistry, this);
    wl_display_roundtrip_queue(m_remote, m_queue);
    if (!m_manager || !m_seat) {
        std::fprintf(stderr, "[raum] the host cannot pass Super through (no shortcut inhibitor)\n");
        return;
    }
    wl_surface* surface = wlr_wl_output_get_surface(output);
    m_inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(m_manager, surface, m_seat);
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_inhibitor), m_queue);
    zwp_keyboard_shortcuts_inhibitor_v1_add_listener(m_inhibitor, &kInhibitor, this);
    wl_display_flush(m_remote);
}

HostShortcuts::~HostShortcuts() {
    if (m_inhibitor) zwp_keyboard_shortcuts_inhibitor_v1_destroy(m_inhibitor);
    if (m_manager) zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(m_manager);
    if (m_seat) wl_seat_destroy(m_seat);
    if (m_registry) wl_registry_destroy(m_registry);
    if (m_queue) wl_event_queue_destroy(m_queue);
}

void HostShortcuts::dispatch() {
    if (m_remote && m_queue) wl_display_dispatch_queue_pending(m_remote, m_queue);
}

} // namespace raum
