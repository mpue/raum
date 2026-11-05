#include "View.hpp"

#include <fitzel/rhi/Device.hpp>

#include "Desk.hpp"
#include "Gpu.hpp"
#include "Server.hpp"

namespace raum {

View::~View() {
    if (canvas.fbo.valid() || canvas.color.valid()) {
        m_server.gpu->makeCurrent();
        if (canvas.fbo.valid()) m_server.gpu->device().destroy(canvas.fbo);
        if (canvas.color.valid()) m_server.gpu->device().destroy(canvas.color);
    }
}

void View::mapped(bool on) {
    if (on == m_mapped) return;
    m_mapped = on;
    if (on) m_server.viewMapped(this);
    else m_server.viewUnmapped(this);
}

// --- xdg-shell -----------------------------------------------------------------------

XdgView::XdgView(Server& server, wlr_xdg_toplevel* toplevel) : View(server), m_toplevel(toplevel) {
    wlr_surface* s = toplevel->base->surface;
    m_map.connect(&s->events.map, this, &XdgView::onMap);
    m_unmap.connect(&s->events.unmap, this, &XdgView::onUnmap);
    m_commit.connect(&s->events.commit, this, &XdgView::onCommit);
    m_destroy.connect(&toplevel->events.destroy, this, &XdgView::onDestroy);
    m_requestMove.connect(&toplevel->events.request_move, this, &XdgView::onRequestMove);
    m_requestResize.connect(&toplevel->events.request_resize, this, &XdgView::onRequestResize);
    m_requestMaximize.connect(&toplevel->events.request_maximize, this, &XdgView::onRequestMaximize);
    m_requestFullscreen.connect(&toplevel->events.request_fullscreen, this, &XdgView::onRequestFullscreen);
    m_setTitle.connect(&toplevel->events.set_title, this, &XdgView::onTitle);
}

XdgView::~XdgView() = default;

std::string XdgView::title() const { return m_toplevel->title ? m_toplevel->title : ""; }
std::string XdgView::appId() const { return m_toplevel->app_id ? m_toplevel->app_id : ""; }

void XdgView::forEachSurface(const std::function<void(wlr_surface*, int, int)>& fn) const {
    wlr_xdg_surface_for_each_surface(
        m_toplevel->base,
        [](wlr_surface* s, int sx, int sy, void* data) {
            (*static_cast<const std::function<void(wlr_surface*, int, int)>*>(data))(s, sx, sy);
        },
        const_cast<std::function<void(wlr_surface*, int, int)>*>(&fn));
}

wlr_surface* XdgView::surfaceAt(double sx, double sy, double& subX, double& subY) const {
    return wlr_xdg_surface_surface_at(m_toplevel->base, sx, sy, &subX, &subY);
}

View* XdgView::parentView() const {
    if (!m_toplevel->parent) return nullptr;
    return m_server.viewOfSurface(m_toplevel->parent->base->surface);
}

void XdgView::setActivated(bool on) {
    if (m_toplevel->base->initialized) wlr_xdg_toplevel_set_activated(m_toplevel, on);
}

void XdgView::requestSize(int width, int height) {
    if (m_toplevel->base->initialized) wlr_xdg_toplevel_set_size(m_toplevel, width, height);
}

void XdgView::setMaximized(bool on) {
    maximized = on;
    if (m_toplevel->base->initialized) wlr_xdg_toplevel_set_maximized(m_toplevel, on);
    forgetAskedSize();
    if (!on) askSize(naturalW, naturalH);
    m_server.scheduleFrame();
}

void XdgView::setFullscreen(bool on) {
    fullscreen = on;
    if (m_toplevel->base->initialized) wlr_xdg_toplevel_set_fullscreen(m_toplevel, on);
    forgetAskedSize();
    if (!on) askSize(naturalW, naturalH);
    m_server.scheduleFrame();
}

void XdgView::close() { wlr_xdg_toplevel_send_close(m_toplevel); }

void XdgView::onCommit(void*) {
    if (m_toplevel->base->initial_commit) {
        // The first commit: let the client pick its size, and say what raum can do.
        wlr_xdg_toplevel_set_wm_capabilities(m_toplevel, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
                                                             WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
        if (Desk* d = m_server.desk.get())
            wlr_xdg_toplevel_set_bounds(m_toplevel, d->width() - 28, d->height() - d->topBar() - 28);
        wlr_xdg_toplevel_set_size(m_toplevel, 0, 0);
        return;
    }
    if (m_mapped && !sizedByDesk && !maximized && !fullscreen) {
        naturalW = m_toplevel->base->geometry.width;
        naturalH = m_toplevel->base->geometry.height;
    }
    m_server.scheduleFrame();
}

void XdgView::onMap(void*) {
    naturalW = m_toplevel->base->geometry.width;
    naturalH = m_toplevel->base->geometry.height;
    mapped(true);
}

void XdgView::onUnmap(void*) { mapped(false); }

void XdgView::onDestroy(void*) {
    mapped(false);
    m_server.destroyView(this);   // deletes this
}

void XdgView::onRequestMove(void*) { m_server.beginMove(this); }

void XdgView::onRequestResize(void* data) {
    auto* e = static_cast<wlr_xdg_toplevel_resize_event*>(data);
    m_server.beginResize(this, e->edges);
}

void XdgView::onRequestMaximize(void*) {
    if (!m_toplevel->base->initialized) return;
    setMaximized(m_toplevel->requested.maximized);
    wlr_xdg_surface_schedule_configure(m_toplevel->base);
}

void XdgView::onRequestFullscreen(void*) {
    if (!m_toplevel->base->initialized) return;
    setFullscreen(m_toplevel->requested.fullscreen);
    wlr_xdg_surface_schedule_configure(m_toplevel->base);
}

void XdgView::onTitle(void*) { m_server.viewTitleChanged(this); }

// --- Popups ----------------------------------------------------------------------------

XdgPopup::XdgPopup(Server& server, wlr_xdg_popup* popup) : m_server(server), m_popup(popup) {
    m_commit.connect(&popup->base->surface->events.commit, this, &XdgPopup::onCommit);
    m_destroy.connect(&popup->events.destroy, this, &XdgPopup::onDestroy);
}

void XdgPopup::onCommit(void*) {
    if (m_popup->base->initial_commit) {
        // Keep it on the screen: the box is the output, in the coordinates of the
        // toplevel's main surface, from where that window stands right now.
        wlr_xdg_surface* root = m_popup->base;
        while (root->role == WLR_XDG_SURFACE_ROLE_POPUP && root->popup && root->popup->parent) {
            wlr_xdg_surface* p = wlr_xdg_surface_try_from_wlr_surface(root->popup->parent);
            if (!p) break;
            root = p;
        }
        View* view = root->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL
                         ? m_server.viewOfSurface(root->surface) : nullptr;
        if (view && m_server.desk) {
            const wlr_box g = view->geometry();
            // wlroots takes the box in the toplevel's surface coordinates and
            // works out the nesting itself.
            const int originX = view->screenX() - g.x, originY = view->screenY() - g.y;
            wlr_box box{-originX, -originY + m_server.desk->topBar(), m_server.desk->width(),
                        m_server.desk->height() - m_server.desk->topBar()};
            wlr_xdg_popup_unconstrain_from_box(m_popup, &box);
        }
        wlr_xdg_surface_schedule_configure(m_popup->base);
    }
    m_server.scheduleFrame();
}

void XdgPopup::onDestroy(void*) {
    m_commit.disconnect();
    m_destroy.disconnect();
    m_server.popupDestroyed(this);   // deletes this
}

} // namespace raum
