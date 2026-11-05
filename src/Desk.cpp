#include "Desk.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "Hud.hpp"
#include "Server.hpp"
#include "State.hpp"

namespace raum {

namespace {

// One colour per room, in the order rooms are made: muted enough to sit behind
// work, distinct enough to tell rooms apart at a glance.
const glm::vec3 kAccents[] = {
    {0.25f, 0.72f, 0.69f},   // teal
    {0.91f, 0.64f, 0.24f},   // amber
    {0.56f, 0.49f, 0.75f},   // violet
    {0.91f, 0.44f, 0.32f},   // coral
    {0.29f, 0.56f, 0.85f},   // sky
    {0.50f, 0.69f, 0.41f},   // moss
    {0.82f, 0.43f, 0.56f},   // rose
    {0.79f, 0.70f, 0.49f},   // sand
};

constexpr int   kGap      = 14;     // between tiled windows and the screen edge
constexpr float kRate     = 13.0f;  // how quickly windows settle (1/s)
constexpr float kCamRate  = 7.5f;

float approach(float cur, float target, float k) { return target + (cur - target) * k; }
glm::vec3 approach(const glm::vec3& cur, const glm::vec3& target, float k) {
    return target + (cur - target) * k;
}

// Shortest way round for an angle.
float approachAngle(float cur, float target, float k) {
    float d = std::remainder(target - cur, 6.2831853f);
    return target - d * k;
}

bool settled(const Pose& a, const Pose& b) {
    return glm::length(a.center - b.center) < 0.02f && std::abs(a.scale - b.scale) < 1e-5f &&
           std::abs(a.yaw - b.yaw) < 1e-4f && std::abs(a.dim - b.dim) < 2e-3f &&
           std::abs(a.alpha - b.alpha) < 2e-3f;
}

} // namespace

Desk::Desk(Server& server) : m_server(server) {
    rooms.push_back(Room{"Start", kAccents[0]});
}

double Desk::wallNow() const {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

int Desk::topBar() const {
    if (View* f = m_server.focused(); f && f->fullscreen && level == Level::Work) return 0;
    return 34;
}

float Desk::floorY() const { return -m_h * 0.5f - 70.0f; }
// Far enough apart that at work a neighbour's windows never show at the edge
// of the screen, near enough that the house view shows every room at a size
// that can be read.
float Desk::roomPitch() const { return level == Level::House ? m_w * 0.78f : m_w * 1.9f; }
float Desk::roomX(int index) const { return static_cast<float>(index) * roomPitch(); }

// --- Rooms -------------------------------------------------------------------------

std::vector<View*> Desk::history(int room) const {
    std::vector<View*> out;
    for (const auto& v : m_server.views)
        if (v->mapped() && v->room == room && !v->parentView()) out.push_back(v.get());
    std::stable_sort(out.begin(), out.end(),
                     [](View* a, View* b) { return a->lastFocus > b->lastFocus; });
    return out;
}

int Desk::windowCount(int room) const { return static_cast<int>(history(room).size()); }

View* Desk::mostRecent(int room, const View* except) const {
    for (View* v : history(room))
        if (v != except) return v;
    return nullptr;
}

void Desk::goToRoom(int index) {
    index = std::clamp(index, 0, static_cast<int>(rooms.size()) - 1);
    const bool changed = index != current;
    current = index;
    m_dollyTarget = 0.0f;
    m_animating = true;
    View* f = m_server.focused();
    if (!f || f->room != index) m_server.focus(mostRecent(index));
    if (changed) {
        m_server.hud->toast(rooms[index].name);
        m_server.state->save();
    }
    m_server.scheduleFrame();
}

void Desk::stepRoom(int dir) {
    const int next = current + dir;
    if (next < 0 || next >= static_cast<int>(rooms.size())) {
        m_server.hud->toast(dir > 0 ? "Letzter Raum -- Super+N macht einen neuen"
                                    : "Erster Raum");
        return;
    }
    goToRoom(next);
}

int Desk::newRoom(const std::string& name) {
    Room r;
    r.name = name.empty() ? "Raum " + std::to_string(rooms.size() + 1) : name;
    r.accent = kAccents[rooms.size() % (sizeof(kAccents) / sizeof(kAccents[0]))];
    rooms.push_back(r);
    m_server.state->save();
    return static_cast<int>(rooms.size()) - 1;
}

void Desk::renameRoom(int index, const std::string& name) {
    if (index < 0 || index >= static_cast<int>(rooms.size()) || name.empty()) return;
    rooms[index].name = name;
    m_server.hud->toast(name);
    m_server.state->save();
}

void Desk::deleteRoomIfEmpty(int index) {
    if (rooms.size() <= 1 || index < 0 || index >= static_cast<int>(rooms.size())) return;
    for (const auto& v : m_server.views)
        if (v->mapped() && v->room == index) {
            m_server.hud->toast("Nur leere Räume lassen sich entfernen");
            return;
        }
    const std::string name = rooms[index].name;
    rooms.erase(rooms.begin() + index);
    for (auto& v : m_server.views)
        if (v->room > index) --v->room;
    if (current >= static_cast<int>(rooms.size())) current = static_cast<int>(rooms.size()) - 1;
    else if (current > index) --current;
    m_server.hud->toast("„" + name + "“ entfernt");
    m_server.state->save();
    m_animating = true;
}

void Desk::moveViewToRoom(View* view, int room, bool follow) {
    if (!view) return;
    while (View* p = view->parentView()) view = p;
    if (room < 0 || room >= static_cast<int>(rooms.size()) || room == view->room) return;
    for (auto& v : m_server.views) {
        View* root = v.get();
        while (View* p = root->parentView()) root = p;
        if (root == view) v->room = room;
    }
    view->slot = -1;
    m_animating = true;
    if (follow) {
        current = room;
        m_server.focus(view);
    } else if (m_server.focused() && m_server.focused()->room != current) {
        m_server.focus(mostRecent(current));
    }
    m_server.hud->toast("→ " + rooms[room].name);
    m_server.scheduleFrame();
}

void Desk::setLevel(Level l) {
    if (l == level) return;
    level = l;
    m_dollyTarget = 0.0f;
    m_animating = true;
    m_server.scheduleFrame();
}

void Desk::zoom(int dir) {
    const int l = std::clamp(static_cast<int>(level) + dir, 0, 2);
    setLevel(static_cast<Level>(l));
}

void Desk::dolly(float steps) {
    if (level != Level::Room) return;
    m_dollyTarget = std::clamp(m_dollyTarget + steps * 700.0f, -1200.0f, 30000.0f);
    m_animating = true;
    m_server.scheduleFrame();
}

void Desk::cycle(int dir) {
    std::vector<View*> h = history(current);
    if (h.size() < 2) return;
    // The window furthest back for -1, the one right behind for +1: one press
    // brings the previous window forward, pressing again goes on back in time
    // because the one that was in front now counts as the most recent.
    View* next = dir > 0 ? h[static_cast<std::size_t>(std::min<int>(rooms[current].split,
                                                                     static_cast<int>(h.size()) - 1))]
                         : h.back();
    m_server.focus(next);
}

void Desk::setSplit(int n) {
    Room& r = rooms[current];
    r.split = std::clamp(n, 1, 3);
    m_animating = true;
    m_server.hud->toast(r.split == 1 ? "Ein Fenster vorn"
                                     : std::to_string(r.split) + " Fenster nebeneinander");
    m_server.state->save();
    m_server.scheduleFrame();
}

// --- Told by the server -------------------------------------------------------------

void Desk::viewMapped(View* view) {
    View* parent = view->parentView();
    view->room = parent ? parent->room : current;
    view->lastFocus = m_server.now();
    view->lastFocusWall = wallNow();
    view->poseValid = false;
    view->appear = 0.0f;
    m_animating = true;
}

void Desk::viewUnmapped(View* view) {
    if (hovered == view) hovered = nullptr;
    if (dragging == view) dragging = nullptr;
    view->slot = -1;
    m_animating = true;
}

void Desk::focused(View* view) {
    if (!view) return;
    View* root = view;
    while (View* p = root->parentView()) root = p;
    root->lastFocus = view->lastFocus = m_server.now();
    root->lastFocusWall = view->lastFocusWall = wallNow();
    if (root->room != current) {
        current = root->room;
        m_dollyTarget = 0.0f;
        m_server.hud->toast(rooms[current].name);
        m_server.state->save();
    }
    m_animating = true;
}

// --- Layout --------------------------------------------------------------------------

Pose Desk::exactPose(double x0, double y0, int w, int h, float z) const {
    // The camera at Level::Work is at (roomX, 0, D) looking down -Z; a plane z
    // nearer than the front one is scaled down by (D - z) / D so it still covers
    // exactly the same pixels.
    const double s = (m_D - z) / m_D;
    Pose p;
    p.center = glm::vec3(static_cast<float>(roomX(current) + (x0 + w * 0.5 - m_w * 0.5) * s),
                         static_cast<float>((m_h * 0.5 - (y0 + h * 0.5)) * s), z);
    p.scale = static_cast<float>(s);
    return p;
}

void Desk::assignSlots(int room, const std::vector<View*>& order) {
    const int n = std::min<int>(rooms[room].split, static_cast<int>(order.size()));
    std::vector<bool> taken(static_cast<std::size_t>(n), false);
    // The front windows keep the slot they had, so bringing one forward never
    // makes the other jump sides.
    for (int i = 0; i < n; ++i) {
        View* v = order[static_cast<std::size_t>(i)];
        if (v->slot >= 0 && v->slot < n && !taken[static_cast<std::size_t>(v->slot)])
            taken[static_cast<std::size_t>(v->slot)] = true;
        else
            v->slot = -2;   // needs one
    }
    for (int i = 0; i < n; ++i) {
        View* v = order[static_cast<std::size_t>(i)];
        if (v->slot != -2) continue;
        for (int s = 0; s < n; ++s)
            if (!taken[static_cast<std::size_t>(s)]) {
                v->slot = s;
                taken[static_cast<std::size_t>(s)] = true;
                break;
            }
    }
    for (std::size_t i = static_cast<std::size_t>(n); i < order.size(); ++i) order[i]->slot = -1;
}

void Desk::layoutRoom(int r) {
    std::vector<View*> order = history(r);
    if (order.empty()) return;
    Room& room = rooms[static_cast<std::size_t>(r)];
    const int n = std::min<int>(room.split, static_cast<int>(order.size()));
    assignSlots(r, order);
    const float rx = roomX(r);
    const int top = topBar();
    const int workH = m_h - top;
    View* focus = m_server.focused();
    View* focusRoot = focus;
    while (focusRoot && focusRoot->parentView()) focusRoot = focusRoot->parentView();

    // --- The front: windows at 1:1 ---------------------------------------------------
    for (int i = 0; i < n; ++i) {
        View* v = order[static_cast<std::size_t>(i)];
        const wlr_box g = v->geometry();
        int gw = std::max(1, g.width), gh = std::max(1, g.height);
        double x0 = 0, y0 = 0;
        if (v->fullscreen) {
            v->askSize(m_w, m_h);
            x0 = (m_w - gw) / 2;
            y0 = (m_h - gh) / 2;
        } else if (n == 1) {
            if (v->sizedByDesk) {
                v->sizedByDesk = false;
                v->askSize(v->naturalW, v->naturalH);
            }
            if (v->maximized) {
                v->askSize(m_w, workH);
                x0 = 0;
                y0 = top;
            } else {
                // A window bigger than the screen is asked to fit, once.
                if (gw > m_w - 2 * kGap || gh > workH - 2 * kGap)
                    v->askSize(std::min(gw, m_w - 2 * kGap), std::min(gh, workH - 2 * kGap));
                x0 = std::floor((m_w - gw) * 0.5 + v->userOffset.x + 0.5);
                y0 = std::floor(top + (workH - gh) * 0.5 + v->userOffset.y + 0.5);
            }
        } else {
            const int sw = (m_w - (n + 1) * kGap) / n;
            const int sh = workH - 2 * kGap;
            if (!v->sizedByDesk) {
                v->naturalW = gw;
                v->naturalH = gh;
                v->sizedByDesk = true;
            }
            v->askSize(sw, sh);
            x0 = kGap + v->slot * (sw + kGap) + std::floor((sw - std::min(gw, sw)) * 0.5);
            y0 = top + kGap + std::floor((sh - std::min(gh, sh)) * 0.5);
        }
        Pose p;
        if (level == Level::Work || r == current) {
            p = exactPose(x0, y0, gw, gh, 0.0f);
            p.center.x += rx - roomX(current);
        }
        p.dim = (n > 1 && v != focusRoot) ? 0.10f : 0.0f;
        p.alpha = 1.0f;
        v->target = p;
        if (r == current) v->placedOnScreen(static_cast<int>(x0), static_cast<int>(y0));
    }
    // A window that leaves the front row gets its own size back: tiled sizes
    // belong to the row, not to the window.
    for (std::size_t k = static_cast<std::size_t>(n); k < order.size(); ++k) {
        View* v = order[k];
        if (v->sizedByDesk && v->naturalW > 0 && v->naturalH > 0) {
            v->sizedByDesk = false;
            v->askSize(v->naturalW, v->naturalH);
        }
    }

    // Only the fullscreen window, nothing behind it; another room's windows
    // only once the camera has stepped back far enough to see that room.
    const bool fs = order[0]->fullscreen && level == Level::Work;
    const bool shown = level == Level::House || r == current;

    if (level == Level::House) {
        // A diorama per room: its windows in a small stack, the one in use in
        // front, small enough that the whole house fits on the screen.
        for (std::size_t k = 0; k < order.size(); ++k) {
            View* v = order[k];
            const wlr_box g = v->geometry();
            const float gw = static_cast<float>(std::max(1, g.width));
            const float gh = static_cast<float>(std::max(1, g.height));
            const float sc = std::min({0.62f, 820.0f / gh, 1250.0f / gw});
            const int ki = static_cast<int>(k);
            const float side = (k % 2 == 0) ? -1.0f : 1.0f;
            Pose p;
            p.center = glm::vec3(rx + (k == 0 ? 0.0f : side * (150.0f + 60.0f * std::min(ki, 4))),
                                 floorY() + gh * sc * 0.5f + 24.0f, -320.0f * ki);
            p.scale = sc;
            p.yaw = k == 0 ? 0.0f : -side * 0.22f;
            p.dim = k < static_cast<std::size_t>(n) ? 0.0f : std::min(0.16f + 0.04f * ki, 0.45f);
            p.alpha = 1.0f;
            v->target = p;
        }
        return;
    }
    if (level == Level::Room) {
        // A gallery: every window of the room stands along an aisle, left and
        // right in turn, turned towards it -- the one in use nearest, and each
        // further back the longer ago it was used. Depth is time: two windows
        // used minutes apart stand close together, one from this morning far
        // behind.
        float z = 0.0f;
        double prevWall = order[0]->lastFocusWall;
        for (std::size_t k = 0; k < order.size(); ++k) {
            View* v = order[k];
            const wlr_box g = v->geometry();
            const float gw = static_cast<float>(std::max(1, g.width));
            const float gh = static_cast<float>(std::max(1, g.height));
            const double gapMin = std::max(0.0, (prevWall - v->lastFocusWall) / 60.0);
            prevWall = v->lastFocusWall;
            if (k > 0) z -= 300.0f + 260.0f * static_cast<float>(std::log2(1.0 + gapMin));
            // Big windows are shown smaller, so a room of mixed sizes reads evenly.
            const float sc = std::min(0.72f, 760.0f / gh);
            const float side = (k % 2 == 0) ? -1.0f : 1.0f;
            const float yaw = -side * 0.36f;
            // Each pair a little wider than the one before, so the ones behind
            // still show past the ones in front.
            const float spread = 110.0f * static_cast<float>(k / 2);
            Pose p;
            p.center = glm::vec3(rx + side * (280.0f + spread + gw * sc * 0.5f * std::cos(yaw)),
                                 floorY() + gh * sc * 0.5f + 30.0f, z - gw * sc * 0.5f * std::sin(0.36f));
            p.scale = sc;
            p.yaw = yaw;
            p.dim = k < static_cast<std::size_t>(n) ? 0.0f : std::min(0.14f + 0.03f * k, 0.42f);
            p.alpha = shown ? 1.0f : 0.0f;
            v->target = p;
        }
        return;
    }

    // --- At work: the history waits in a deck behind the front window ---------------
    float z = -380.0f;
    for (std::size_t k = static_cast<std::size_t>(n); k < order.size(); ++k) {
        View* v = order[k];
        const int rank = static_cast<int>(k) - n + 1;
        if (rank > 1) z -= 260.0f;
        const float side = (rank % 2) ? -1.0f : 1.0f;
        Pose p;
        p.center = glm::vec3(rx + side * std::min(70.0f * rank, 280.0f), 40.0f * std::min(rank, 4), z);
        p.scale = 1.0f;
        p.yaw = 0.0f;
        p.dim = std::min(0.55f + 0.06f * rank, 0.85f);
        p.alpha = (fs || !shown) ? 0.0f : 1.0f;
        v->target = p;
    }
    if (!shown)
        for (int i = 0; i < n; ++i) order[static_cast<std::size_t>(i)]->target.alpha = 0.0f;
}

void Desk::placeChildren() {
    // Dialogs and other windows with a parent: centred over it, a hair nearer.
    for (const auto& up : m_server.views) {
        View* v = up.get();
        if (!v->mapped() || !v->parentView()) continue;
        int depth = 0;
        View* root = v;
        while (View* p = root->parentView()) {
            root = p;
            ++depth;
        }
        v->room = root->room;
        v->slot = root->slot >= 0 ? root->slot : -1;
        const wlr_box g = v->geometry();
        const wlr_box rg = root->geometry();
        const int gw = std::max(1, g.width), gh = std::max(1, g.height);
        Pose p = root->target;
        if (level == Level::Work && root->slot >= 0 && root->room == current) {
            const double x0 = std::floor(root->screenX() + (rg.width - gw) * 0.5 + v->userOffset.x);
            const double y0 = std::floor(std::max<double>(topBar(),
                                         root->screenY() + (rg.height - gh) * 0.5 + v->userOffset.y));
            p = exactPose(x0, y0, gw, gh, 3.0f * depth);
            p.dim = root->target.dim;
            v->placedOnScreen(static_cast<int>(x0), static_cast<int>(y0));
        } else {
            const glm::vec3 n(std::sin(p.yaw), 0.0f, std::cos(p.yaw));
            p.center += n * (4.0f * depth);
        }
        v->target = p;
    }
}

void Desk::layout() {
    for (int r = 0; r < static_cast<int>(rooms.size()); ++r) layoutRoom(r);
    placeChildren();
}

void Desk::cameraTarget(glm::vec3& pos, glm::vec3& look) const {
    const float rx = roomX(current);
    switch (level) {
        case Level::Work:
            pos = glm::vec3(rx, 0.0f, m_D);
            look = glm::vec3(rx, 0.0f, 0.0f);
            break;
        case Level::Room:
            pos = glm::vec3(rx, floorY() + m_h * 0.95f, m_D + 650.0f - m_dolly);
            look = glm::vec3(rx, floorY() + m_h * 0.32f, -1900.0f - m_dolly);
            break;
        case Level::House: {
            const float x0 = roomX(0), x1 = roomX(static_cast<int>(rooms.size()) - 1);
            const float cx = 0.5f * (x0 + x1);
            const float span = (x1 - x0) + roomPitch() * 1.0f;
            const float aspect = static_cast<float>(m_w) / static_cast<float>(m_h);
            const float dist = std::max(span / (2.0f * std::tan(glm::radians(m_fovY) * 0.5f) * aspect),
                                        m_D * 1.3f);
            pos = glm::vec3(cx, floorY() + dist * 0.30f, dist * 0.80f);
            look = glm::vec3(cx, floorY() + m_h * 0.30f, -600.0f);
            break;
        }
    }
}

void Desk::update(double now, double dt, int width, int height) {
    m_now = now;
    if (width != m_w || height != m_h) {
        m_w = std::max(1, width);
        m_h = std::max(1, height);
        m_animating = true;
    }
    m_D = static_cast<float>(m_h * 0.5 / std::tan(glm::radians(m_fovY) * 0.5));
    dt = std::clamp(dt, 0.0, 0.1);
    const float k = std::exp(-kRate * static_cast<float>(dt));
    const float kc = std::exp(-kCamRate * static_cast<float>(dt));

    m_dolly = approach(m_dolly, m_dollyTarget, kc);
    if (std::abs(m_dolly - m_dollyTarget) < 0.5f) m_dolly = m_dollyTarget;
    layout();

    bool moving = std::abs(m_dolly - m_dollyTarget) > 0.0f;
    cameraTarget(m_camPosTarget, m_camLookTarget);
    if (!m_camValid) {
        m_camPos = m_camPosTarget;
        m_camLook = m_camLookTarget;
        m_camValid = true;
    }
    m_camPos = approach(m_camPos, m_camPosTarget, kc);
    m_camLook = approach(m_camLook, m_camLookTarget, kc);
    if (glm::length(m_camPos - m_camPosTarget) < 0.02f && glm::length(m_camLook - m_camLookTarget) < 0.02f) {
        m_camPos = m_camPosTarget;
        m_camLook = m_camLookTarget;
    } else {
        moving = true;
    }

    for (const auto& up : m_server.views) {
        View* v = up.get();
        if (!v->mapped()) continue;
        if (!v->poseValid) {
            // New windows rise out of the depth into place.
            v->pose = v->target;
            v->pose.center.z -= 260.0f;
            v->pose.alpha = 0.0f;
            v->poseValid = true;
        }
        Pose& p = v->pose;
        const Pose& t = v->target;
        p.center = approach(p.center, t.center, k);
        p.scale = approach(p.scale, t.scale, k);
        p.yaw = approachAngle(p.yaw, t.yaw, k);
        p.dim = approach(p.dim, t.dim, k);
        p.alpha = approach(p.alpha, t.alpha, k);
        if (settled(p, t)) p = t;
        else moving = true;
    }
    m_animating = moving;

    const float aspect = static_cast<float>(m_w) / static_cast<float>(m_h);
    m_view = glm::lookAt(m_camPos, m_camLook, glm::vec3(0.0f, 1.0f, 0.0f));
    m_proj = glm::perspective(glm::radians(m_fovY), aspect, 20.0f, 400000.0f);

    // What to draw, back to front: everything that is on the screen at all.
    m_drawList.clear();
    for (const auto& up : m_server.views) {
        View* v = up.get();
        if (!v->mapped() || v->pose.alpha < 0.004f) continue;
        glm::vec3 c[4];
        canvasQuad(v, v->pose, c);
        bool left = true, right = true, above = true, below = true, behind = true;
        for (const glm::vec3& w : c) {
            const glm::vec3 s = toScreen(w);
            if (s.z > 0.0f) {
                behind = false;
                left = left && s.x < 0.0f;
                right = right && s.x > m_w;
                above = above && s.y < 0.0f;
                below = below && s.y > m_h;
            }
        }
        if (behind || left || right || above || below) continue;
        m_drawList.push_back(v);
    }
    const glm::vec3 eye = m_camPos;
    std::stable_sort(m_drawList.begin(), m_drawList.end(), [&](View* a, View* b) {
        const float da = glm::dot(a->pose.center - eye, a->pose.center - eye);
        const float db = glm::dot(b->pose.center - eye, b->pose.center - eye);
        if (std::abs(da - db) > 1.0f) return da > db;
        return a->pose.center.z < b->pose.center.z;
    });
}

// --- Geometry ------------------------------------------------------------------------

glm::vec3 Desk::toScreen(const glm::vec3& world) const {
    const glm::vec4 clip = m_proj * m_view * glm::vec4(world, 1.0f);
    if (clip.w <= 1e-4f) return glm::vec3(0.0f, 0.0f, -1.0f);
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    return glm::vec3((ndc.x * 0.5f + 0.5f) * m_w, (0.5f - ndc.y * 0.5f) * m_h, clip.w);
}

void Desk::canvasQuad(const View* view, const Pose& pose, glm::vec3 out[4]) const {
    const wlr_box g = view->geometry();
    wlr_box b = view->canvas.valid() ? view->canvas.box : g;
    if (b.width <= 0 || b.height <= 0) b = g;
    const double cx = g.x + g.width * 0.5, cy = g.y + g.height * 0.5;
    const float lx0 = static_cast<float>(b.x - cx), lx1 = static_cast<float>(b.x + b.width - cx);
    const float ly0 = static_cast<float>(b.y - cy), ly1 = static_cast<float>(b.y + b.height - cy);
    const glm::vec3 R(std::cos(pose.yaw), 0.0f, -std::sin(pose.yaw));
    const glm::vec3 U(0.0f, 1.0f, 0.0f);
    auto corner = [&](float lx, float ly) { return pose.center + R * (lx * pose.scale) - U * (ly * pose.scale); };
    out[0] = corner(lx0, ly0);
    out[1] = corner(lx1, ly0);
    out[2] = corner(lx1, ly1);
    out[3] = corner(lx0, ly1);
}

bool Desk::pointOnView(const View* view, double x, double y, double& mx, double& my) const {
    const glm::dmat4 inv = glm::inverse(glm::dmat4(m_proj) * glm::dmat4(m_view));
    const double nx = 2.0 * x / m_w - 1.0, ny = 1.0 - 2.0 * y / m_h;
    glm::dvec4 a = inv * glm::dvec4(nx, ny, -1.0, 1.0);
    glm::dvec4 b = inv * glm::dvec4(nx, ny, 1.0, 1.0);
    const glm::dvec3 o = glm::dvec3(a) / a.w;
    const glm::dvec3 dir = glm::normalize(glm::dvec3(b) / b.w - o);
    const Pose& p = view->pose;
    const glm::dvec3 c(p.center);
    const glm::dvec3 N(std::sin(p.yaw), 0.0, std::cos(p.yaw));
    const glm::dvec3 R(std::cos(p.yaw), 0.0, -std::sin(p.yaw));
    const double denom = glm::dot(dir, N);
    if (std::abs(denom) < 1e-9) return false;
    const double t = glm::dot(c - o, N) / denom;
    if (t <= 0.0) return false;
    const glm::dvec3 hit = o + dir * t;
    const double lx = glm::dot(hit - c, R) / p.scale;
    const double ly = -(hit.y - c.y) / p.scale;
    const wlr_box g = view->geometry();
    mx = g.x + g.width * 0.5 + lx;
    my = g.y + g.height * 0.5 + ly;
    return true;
}

bool Desk::frontRect(const View* view, wlr_box& out) const {
    if (view->slot < 0 || view->room != current) return false;
    const wlr_box g = view->geometry();
    out = wlr_box{view->screenX(), view->screenY(), g.width, g.height};
    return true;
}

Desk::Hit Desk::pick(double x, double y) const {
    Hit best;
    // Front to back: the first window the ray meets that has something there.
    for (auto it = m_drawList.rbegin(); it != m_drawList.rend(); ++it) {
        View* v = *it;
        if (v->pose.alpha < 0.5f || v == dragging) continue;
        double mx = 0, my = 0;
        if (!pointOnView(v, x, y, mx, my)) continue;
        wlr_box b = v->canvas.valid() ? v->canvas.box : v->geometry();
        if (mx < b.x || my < b.y || mx >= b.x + b.width || my >= b.y + b.height) continue;
        View* root = v;
        while (View* p = root->parentView()) root = p;
        const bool front = level == Level::Work && root->slot >= 0 && root->room == current;
        if (front) {
            double sx = 0, sy = 0;
            wlr_surface* s = v->surfaceAt(mx, my, sx, sy);
            if (!s) continue;   // a transparent margin: look behind it
            best = Hit{v, s, sx, sy, mx, my, true};
            return best;
        }
        const wlr_box g = v->geometry();
        if (mx < g.x || my < g.y || mx >= g.x + g.width || my >= g.y + g.height) continue;
        return Hit{v, nullptr, 0, 0, mx, my, false};
    }
    return best;
}

int Desk::pickRoom(double x, double y) const {
    const glm::dmat4 inv = glm::inverse(glm::dmat4(m_proj) * glm::dmat4(m_view));
    const double nx = 2.0 * x / m_w - 1.0, ny = 1.0 - 2.0 * y / m_h;
    glm::dvec4 a = inv * glm::dvec4(nx, ny, -1.0, 1.0);
    glm::dvec4 b = inv * glm::dvec4(nx, ny, 1.0, 1.0);
    const glm::dvec3 o = glm::dvec3(a) / a.w;
    const glm::dvec3 dir = glm::normalize(glm::dvec3(b) / b.w - o);
    if (std::abs(dir.y) < 1e-9) return -1;
    const double t = (floorY() - o.y) / dir.y;
    if (t <= 0) return -1;
    const double wx = o.x + dir.x * t;
    const double pitch = roomPitch();
    const int idx = static_cast<int>(std::floor(wx / pitch + 0.5));
    if (idx < 0 || idx >= static_cast<int>(rooms.size())) return -1;
    return idx;
}

} // namespace raum
