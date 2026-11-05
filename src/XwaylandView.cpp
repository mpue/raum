#include "Desk.hpp"
#include "Server.hpp"
#include "View.hpp"

namespace raum {

namespace {

// The managed window an override-redirect one (a menu) belongs to: its X parent
// chain, else whichever X11 window has the focus.
View* ownerOf(Server& server, wlr_xwayland_surface* xs) {
    for (wlr_xwayland_surface* p = xs->parent; p; p = p->parent)
        if (p->surface)
            if (View* v = server.viewOfSurface(p->surface)) return v;
    View* f = server.focused();
    return (f && dynamic_cast<XwaylandView*>(f)) ? f : nullptr;
}

} // namespace

// --- Managed windows -----------------------------------------------------------------

XwaylandView::XwaylandView(Server& server, wlr_xwayland_surface* xs) : View(server), m_xsurface(xs) {
    m_associate.connect(&xs->events.associate, this, &XwaylandView::onAssociate);
    m_dissociate.connect(&xs->events.dissociate, this, &XwaylandView::onDissociate);
    m_destroy.connect(&xs->events.destroy, this, &XwaylandView::onDestroy);
    m_requestConfigure.connect(&xs->events.request_configure, this, &XwaylandView::onRequestConfigure);
    m_requestMaximize.connect(&xs->events.request_maximize, this, &XwaylandView::onRequestMaximize);
    m_requestFullscreen.connect(&xs->events.request_fullscreen, this, &XwaylandView::onRequestFullscreen);
    m_requestActivate.connect(&xs->events.request_activate, this, &XwaylandView::onRequestActivate);
    m_setTitle.connect(&xs->events.set_title, this, &XwaylandView::onTitle);
    if (xs->surface) onAssociate(nullptr);
}

XwaylandView::~XwaylandView() = default;

std::string XwaylandView::title() const { return m_xsurface->title ? m_xsurface->title : ""; }
std::string XwaylandView::appId() const { return m_xsurface->class_ ? m_xsurface->class_ : ""; }

wlr_box XwaylandView::geometry() const {
    wlr_surface* s = m_xsurface->surface;
    return wlr_box{0, 0, s ? s->current.width : m_xsurface->width, s ? s->current.height : m_xsurface->height};
}

void XwaylandView::forEachSurface(const std::function<void(wlr_surface*, int, int)>& fn) const {
    if (!m_xsurface->surface) return;
    wlr_surface_for_each_surface(
        m_xsurface->surface,
        [](wlr_surface* s, int sx, int sy, void* data) {
            (*static_cast<const std::function<void(wlr_surface*, int, int)>*>(data))(s, sx, sy);
        },
        const_cast<std::function<void(wlr_surface*, int, int)>*>(&fn));
    // Its menus and tooltips, where X put them relative to this window.
    for (const auto& u : m_server.unmanaged) {
        if (!u->mapped() || !u->xsurface()->surface) continue;
        if (ownerOf(m_server, u->xsurface()) != this) continue;
        struct Shifted {
            const std::function<void(wlr_surface*, int, int)>* fn;
            int ox, oy;
        } shifted{&fn, u->xsurface()->x - m_xsurface->x, u->xsurface()->y - m_xsurface->y};
        wlr_surface_for_each_surface(
            u->xsurface()->surface,
            [](wlr_surface* s, int sx, int sy, void* data) {
                const auto* d = static_cast<const Shifted*>(data);
                (*d->fn)(s, sx + d->ox, sy + d->oy);
            },
            &shifted);
    }
}

wlr_surface* XwaylandView::surfaceAt(double sx, double sy, double& subX, double& subY) const {
    for (auto it = m_server.unmanaged.rbegin(); it != m_server.unmanaged.rend(); ++it) {
        const auto& u = *it;
        if (!u->mapped() || !u->xsurface()->surface || ownerOf(m_server, u->xsurface()) != this)
            continue;
        const double ox = u->xsurface()->x - m_xsurface->x, oy = u->xsurface()->y - m_xsurface->y;
        if (wlr_surface* s = wlr_surface_surface_at(u->xsurface()->surface, sx - ox, sy - oy, &subX, &subY))
            return s;
    }
    if (!m_xsurface->surface) return nullptr;
    return wlr_surface_surface_at(m_xsurface->surface, sx, sy, &subX, &subY);
}

View* XwaylandView::parentView() const {
    for (wlr_xwayland_surface* p = m_xsurface->parent; p; p = p->parent)
        if (p->surface)
            if (View* v = m_server.viewOfSurface(p->surface)) return v;
    return nullptr;
}

void XwaylandView::setActivated(bool on) {
    wlr_xwayland_surface_activate(m_xsurface, on);
    if (on) wlr_xwayland_surface_restack(m_xsurface, nullptr, XCB_STACK_MODE_ABOVE);
}

void XwaylandView::sendConfigure() {
    if (m_w <= 0 || m_h <= 0) return;
    wlr_xwayland_surface_configure(m_xsurface, static_cast<int16_t>(m_screenX),
                                   static_cast<int16_t>(m_screenY), static_cast<uint16_t>(m_w),
                                   static_cast<uint16_t>(m_h));
}

void XwaylandView::requestSize(int width, int height) {
    if (width <= 0 || height <= 0) return;   // X has no "your choice"
    m_w = width;
    m_h = height;
    sendConfigure();
}

void XwaylandView::placedOnScreen(int x, int y) {
    if (x == m_screenX && y == m_screenY) return;
    m_screenX = x;
    m_screenY = y;
    // X clients place their menus by the root position they think they have.
    if (m_w <= 0) {
        m_w = m_xsurface->width;
        m_h = m_xsurface->height;
    }
    sendConfigure();
}

void XwaylandView::setMaximized(bool on) {
    maximized = on;
    wlr_xwayland_surface_set_maximized(m_xsurface, on, on);
    forgetAskedSize();
    if (!on) askSize(naturalW, naturalH);
    m_server.scheduleFrame();
}

void XwaylandView::setFullscreen(bool on) {
    fullscreen = on;
    wlr_xwayland_surface_set_fullscreen(m_xsurface, on);
    forgetAskedSize();
    if (!on) askSize(naturalW, naturalH);
    m_server.scheduleFrame();
}

void XwaylandView::close() { wlr_xwayland_surface_close(m_xsurface); }

void XwaylandView::onAssociate(void*) {
    wlr_surface* s = m_xsurface->surface;
    m_map.connect(&s->events.map, this, &XwaylandView::onMap);
    m_unmap.connect(&s->events.unmap, this, &XwaylandView::onUnmap);
    m_commit.connect(&s->events.commit, this, &XwaylandView::onCommit);
}

void XwaylandView::onDissociate(void*) {
    mapped(false);
    m_map.disconnect();
    m_unmap.disconnect();
    m_commit.disconnect();
}

void XwaylandView::onMap(void*) {
    m_w = m_xsurface->width;
    m_h = m_xsurface->height;
    naturalW = m_w;
    naturalH = m_h;
    mapped(true);
}

void XwaylandView::onUnmap(void*) { mapped(false); }

void XwaylandView::onCommit(void*) { m_server.scheduleFrame(); }

void XwaylandView::onDestroy(void*) {
    mapped(false);
    m_server.destroyView(this);   // deletes this
}

void XwaylandView::onRequestConfigure(void* data) {
    auto* e = static_cast<wlr_xwayland_surface_configure_event*>(data);
    // The size the client asks for, the position raum gives it.
    if (!sizedByDesk && !maximized && !fullscreen) {
        m_w = e->width;
        m_h = e->height;
        naturalW = m_w;
        naturalH = m_h;
    }
    if (!m_mapped) {
        m_screenX = e->x;
        m_screenY = e->y;
    }
    wlr_xwayland_surface_configure(m_xsurface, static_cast<int16_t>(m_screenX),
                                   static_cast<int16_t>(m_screenY), static_cast<uint16_t>(m_w),
                                   static_cast<uint16_t>(m_h));
    m_server.scheduleFrame();
}

void XwaylandView::onRequestMaximize(void*) {
    setMaximized(!(m_xsurface->maximized_horz && m_xsurface->maximized_vert) ? true : !maximized);
}

void XwaylandView::onRequestFullscreen(void*) { setFullscreen(m_xsurface->fullscreen); }

void XwaylandView::onRequestActivate(void*) {
    if (m_mapped) m_server.focus(this);
}

void XwaylandView::onTitle(void*) { m_server.viewTitleChanged(this); }

// --- Override-redirect windows -------------------------------------------------------

XwaylandUnmanaged::XwaylandUnmanaged(Server& server, wlr_xwayland_surface* xs)
    : m_server(server), m_xsurface(xs) {
    m_associate.connect(&xs->events.associate, this, &XwaylandUnmanaged::onAssociate);
    m_dissociate.connect(&xs->events.dissociate, this, &XwaylandUnmanaged::onDissociate);
    m_destroy.connect(&xs->events.destroy, this, &XwaylandUnmanaged::onDestroy);
    m_requestConfigure.connect(&xs->events.request_configure, this,
                               &XwaylandUnmanaged::onRequestConfigure);
    if (xs->surface) onAssociate(nullptr);
}

XwaylandUnmanaged::~XwaylandUnmanaged() = default;

void XwaylandUnmanaged::onAssociate(void*) {
    m_map.connect(&m_xsurface->surface->events.map, this, &XwaylandUnmanaged::onMap);
    m_unmap.connect(&m_xsurface->surface->events.unmap, this, &XwaylandUnmanaged::onUnmap);
}

void XwaylandUnmanaged::onDissociate(void*) {
    m_mapped = false;
    m_map.disconnect();
    m_unmap.disconnect();
}

void XwaylandUnmanaged::onMap(void*) {
    m_mapped = true;
    m_server.scheduleFrame();
}

void XwaylandUnmanaged::onUnmap(void*) {
    m_mapped = false;
    m_server.scheduleFrame();
}

void XwaylandUnmanaged::onDestroy(void*) { m_server.unmanagedDestroyed(this); }

void XwaylandUnmanaged::onRequestConfigure(void* data) {
    auto* e = static_cast<wlr_xwayland_surface_configure_event*>(data);
    wlr_xwayland_surface_configure(m_xsurface, e->x, e->y, e->width, e->height);
}

} // namespace raum
