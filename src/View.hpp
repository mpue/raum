#pragma once

#include <functional>
#include <string>

#include <glm/glm.hpp>

#include <fitzel/rhi/Types.hpp>

#include "wlr.hpp"

namespace raum {

class Server;

// Where a window is in the room and how it looks there. `center` is the middle
// of its window geometry (the part without client-drawn shadows) in world
// units, `scale` world units per window pixel, `yaw` a turn about the vertical.
struct Pose {
    glm::vec3 center{0.0f};
    float     scale = 1.0f;
    float     yaw   = 0.0f;
    float     dim   = 0.0f;   // 0 = as is, 1 = gone into the background
    float     alpha = 1.0f;
};

// The window's content composed into one image: the main surface, its
// subsurfaces and its popups, at their offsets. Drawn into the room as one
// textured quad; rebuilt only when something in it committed.
struct Canvas {
    fitzel::rhi::TextureHandle  color;
    fitzel::rhi::FramebufferHandle fbo;
    int width = 0, height = 0;          // pixels
    wlr_box box{};                      // its extent, in main-surface coordinates
    std::uint64_t contentKey = 0;       // what it was last drawn from
    bool valid() const { return color.valid(); }
};

// A toplevel window, whatever protocol it speaks (xdg-shell or X11 through
// Xwayland). The desk only ever talks to this.
class View {
public:
    explicit View(Server& server) : m_server(server) {}
    virtual ~View();
    View(const View&)            = delete;
    View& operator=(const View&) = delete;

    virtual wlr_surface* surface() const = 0;
    virtual std::string title() const = 0;
    virtual std::string appId() const = 0;
    // The window's geometry in main-surface coordinates: where the window is
    // inside its surface (client-side shadows make the surface bigger).
    virtual wlr_box geometry() const = 0;
    // Every surface that shows, with its offset from the main surface (popups
    // and subsurfaces included), back to front.
    virtual void forEachSurface(const std::function<void(wlr_surface*, int, int)>& fn) const = 0;
    // The surface under a point in main-surface coordinates, and the point in
    // that surface's coordinates.
    virtual wlr_surface* surfaceAt(double sx, double sy, double& subX, double& subY) const = 0;
    virtual View* parentView() const = 0;

    virtual void setActivated(bool on) = 0;
    // Ask the client for a window size (0 x 0: its own choice).
    virtual void requestSize(int width, int height) = 0;
    virtual void setMaximized(bool on) = 0;
    virtual void setFullscreen(bool on) = 0;
    virtual void close() = 0;
    // Where the window's geometry sits on the screen when it is in front, in
    // output pixels: X11 clients place their menus from it, xdg popups are kept
    // on the screen with it.
    virtual void placedOnScreen(int x, int y) { m_screenX = x; m_screenY = y; }

    bool mapped() const { return m_mapped; }
    Server& server() { return m_server; }

    // --- The desk's bookkeeping -------------------------------------------------
    int    room = 0;
    double lastFocus = 0.0;        // CLOCK_MONOTONIC seconds
    double lastFocusWall = 0.0;    // seconds since the epoch, for "vor 5 min"
    int    slot = -1;              // the front slot it fills, -1 = in the history
    bool   maximized = false;
    bool   fullscreen = false;
    // What the client chose before raum asked it for a tiled size, given back
    // when it stands alone again.
    int    naturalW = 0, naturalH = 0;
    bool   sizedByDesk = false;
    // Moved by hand (Super + drag) in its plane, in screen pixels.
    glm::vec2 userOffset{0.0f};
    Pose   pose;                   // as drawn this frame
    Pose   target;                 // where it is heading
    bool   poseValid = false;      // false until the first layout
    float  appear = 0.0f;          // 0 -> 1 as it fades in after mapping
    Canvas canvas;

    int screenX() const { return m_screenX; }
    int screenY() const { return m_screenY; }

    // Ask for a size once: the layout runs every frame, a configure should not.
    void askSize(int width, int height) {
        if (width == m_askedW && height == m_askedH) return;
        m_askedW = width;
        m_askedH = height;
        requestSize(width, height);
    }
    void forgetAskedSize() { m_askedW = m_askedH = -1; }

protected:
    void mapped(bool on);

    Server& m_server;
    bool m_mapped = false;
    int  m_screenX = 0, m_screenY = 0;
    int  m_askedW = -1, m_askedH = -1;
};

// An xdg-shell toplevel.
class XdgView final : public View {
public:
    XdgView(Server& server, wlr_xdg_toplevel* toplevel);
    ~XdgView() override;

    wlr_surface* surface() const override { return m_toplevel->base->surface; }
    std::string title() const override;
    std::string appId() const override;
    wlr_box geometry() const override { return m_toplevel->base->geometry; }
    void forEachSurface(const std::function<void(wlr_surface*, int, int)>& fn) const override;
    wlr_surface* surfaceAt(double sx, double sy, double& subX, double& subY) const override;
    View* parentView() const override;

    void setActivated(bool on) override;
    void requestSize(int width, int height) override;
    void setMaximized(bool on) override;
    void setFullscreen(bool on) override;
    void close() override;

    wlr_xdg_toplevel* toplevel() const { return m_toplevel; }

private:
    void onMap(void*);
    void onUnmap(void*);
    void onCommit(void*);
    void onDestroy(void*);
    void onRequestMove(void*);
    void onRequestResize(void*);
    void onRequestMaximize(void*);
    void onRequestFullscreen(void*);
    void onTitle(void*);

    wlr_xdg_toplevel* m_toplevel;
    Listener<XdgView> m_map, m_unmap, m_commit, m_destroy;
    Listener<XdgView> m_requestMove, m_requestResize, m_requestMaximize, m_requestFullscreen;
    Listener<XdgView> m_setTitle;
};

// An xdg popup (a menu, a tooltip): kept on the screen when it first appears.
class XdgPopup {
public:
    XdgPopup(Server& server, wlr_xdg_popup* popup);

private:
    void onCommit(void*);
    void onDestroy(void*);

    Server& m_server;
    wlr_xdg_popup* m_popup;
    Listener<XdgPopup> m_commit, m_destroy;
};

// An X11 window through Xwayland (XwaylandView.cpp).
class XwaylandView final : public View {
public:
    XwaylandView(Server& server, wlr_xwayland_surface* xsurface);
    ~XwaylandView() override;

    wlr_surface* surface() const override { return m_xsurface->surface; }
    std::string title() const override;
    std::string appId() const override;
    wlr_box geometry() const override;
    void forEachSurface(const std::function<void(wlr_surface*, int, int)>& fn) const override;
    wlr_surface* surfaceAt(double sx, double sy, double& subX, double& subY) const override;
    View* parentView() const override;

    void setActivated(bool on) override;
    void requestSize(int width, int height) override;
    void setMaximized(bool on) override;
    void setFullscreen(bool on) override;
    void close() override;
    void placedOnScreen(int x, int y) override;

    wlr_xwayland_surface* xsurface() const { return m_xsurface; }

private:
    void onAssociate(void*);
    void onDissociate(void*);
    void onMap(void*);
    void onUnmap(void*);
    void onCommit(void*);
    void onDestroy(void*);
    void onRequestConfigure(void*);
    void onRequestMaximize(void*);
    void onRequestFullscreen(void*);
    void onRequestActivate(void*);
    void onTitle(void*);
    void sendConfigure();

    wlr_xwayland_surface* m_xsurface;
    int m_w = 0, m_h = 0;
    Listener<XwaylandView> m_associate, m_dissociate, m_map, m_unmap, m_commit, m_destroy;
    Listener<XwaylandView> m_requestConfigure, m_requestMaximize, m_requestFullscreen;
    Listener<XwaylandView> m_requestActivate, m_setTitle;
};

// An override-redirect X11 window (a menu, a tooltip, a drag icon): it draws
// at a root position its client chose. Shown inside the canvas of the window
// it belongs to, at that position relative to where that window is.
class XwaylandUnmanaged {
public:
    XwaylandUnmanaged(Server& server, wlr_xwayland_surface* xsurface);
    ~XwaylandUnmanaged();
    wlr_xwayland_surface* xsurface() const { return m_xsurface; }
    bool mapped() const { return m_mapped; }

private:
    void onAssociate(void*);
    void onDissociate(void*);
    void onMap(void*);
    void onUnmap(void*);
    void onDestroy(void*);
    void onRequestConfigure(void*);

    Server& m_server;
    wlr_xwayland_surface* m_xsurface;
    bool m_mapped = false;
    Listener<XwaylandUnmanaged> m_associate, m_dissociate, m_map, m_unmap, m_destroy;
    Listener<XwaylandUnmanaged> m_requestConfigure;
};

} // namespace raum
