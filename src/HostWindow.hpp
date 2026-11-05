#pragma once

#include <functional>

struct wl_compositor;
struct wl_display;
struct wl_registry;
struct wl_surface;
struct xdg_surface;
struct xdg_toplevel;
struct xdg_wm_base;
struct wlr_backend;
struct wlr_output;

namespace raum {

// raum's window on the desktop it runs nested in, made by raum rather than by
// wlroots' Wayland backend: the backend keeps the toplevel it makes to itself,
// and with it the one thing raum needs from it -- asking for fullscreen. The
// surface is handed to the backend as an output
// (wlr_wl_output_create_from_surface), so buffers, frame callbacks, input and
// the cursor work as for any nested output.
class HostWindow {
public:
    // `onResize` gets every size the host settles on (0 x 0: raum chooses),
    // `onClose` the window's close button.
    HostWindow(wlr_backend* wlBackend, bool fullscreen, std::function<void(int, int)> onResize,
               std::function<void()> onClose);
    ~HostWindow();
    HostWindow(const HostWindow&)            = delete;
    HostWindow& operator=(const HostWindow&) = delete;

    // Configured by the host and ready to become an output.
    bool ready() const { return m_configured; }
    // Hand the surface to the backend; its new_output signal fires inside.
    void createOutput(wlr_backend* wlBackend);
    // Null until createOutput, or when the host has no xdg-shell (then wlroots
    // makes its own window).
    wlr_output* output() const { return m_output; }
    // The size of the first configure, before the output exists; 0 x 0: none given.
    int initialWidth() const { return m_width; }
    int initialHeight() const { return m_height; }

    void setFullscreen(bool on);
    bool fullscreen() const { return m_fullscreen; }

    // --- For the protocol callbacks ------------------------------------------------
    void configured(int width, int height, bool fullscreen);
    void surfaceConfigure(unsigned serial);
    void closeRequested() { if (m_onClose) m_onClose(); }
    void global(wl_registry* reg, unsigned name, const char* iface, unsigned version);

private:
    wl_display*    m_remote = nullptr;
    wl_registry*   m_registry = nullptr;
    wl_compositor* m_compositor = nullptr;
    xdg_wm_base*   m_wmBase = nullptr;
    wl_surface*    m_surface = nullptr;
    xdg_surface*   m_xdgSurface = nullptr;
    xdg_toplevel*  m_toplevel = nullptr;
    wlr_output*    m_output = nullptr;
    std::function<void(int, int)> m_onResize;
    std::function<void()> m_onClose;
    int  m_width = 0, m_height = 0;           // what the host asked for last
    int  m_pendingW = 0, m_pendingH = 0;
    bool m_configured = false;
    bool m_fullscreen = false;
    bool m_pendingFullscreen = false;
};

} // namespace raum
