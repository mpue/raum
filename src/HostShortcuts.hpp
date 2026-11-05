#pragma once

struct wl_display;
struct wl_event_queue;
struct wl_registry;
struct wl_seat;
struct wlr_backend;
struct wlr_output;
struct zwp_keyboard_shortcuts_inhibit_manager_v1;
struct zwp_keyboard_shortcuts_inhibitor_v1;

namespace raum {

// Nested in another desktop, raum is one window there -- and that desktop keeps
// Super for itself (GNOME opens its overview on it). This asks the host, through
// keyboard-shortcuts-inhibit, to pass its shortcuts through while raum's window
// has the keyboard. GNOME asks the user once whether to allow it.
//
// It speaks to the host on an event queue of its own, so nothing here runs
// inside the wlroots backend's dispatch.
class HostShortcuts {
public:
    HostShortcuts(wlr_backend* backend, wlr_output* output);
    ~HostShortcuts();
    void dispatch();
    bool active() const { return m_active; }
    bool available() const { return m_inhibitor != nullptr; }

    static void onActive(void* data, zwp_keyboard_shortcuts_inhibitor_v1*);
    static void onInactive(void* data, zwp_keyboard_shortcuts_inhibitor_v1*);

    wl_display* m_remote = nullptr;
    wl_event_queue* m_queue = nullptr;
    wl_registry* m_registry = nullptr;
    wl_seat* m_seat = nullptr;
    zwp_keyboard_shortcuts_inhibit_manager_v1* m_manager = nullptr;
    zwp_keyboard_shortcuts_inhibitor_v1* m_inhibitor = nullptr;
    bool m_active = false;
};

} // namespace raum
