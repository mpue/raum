// Keys and the pointer: what raum keeps for itself (Super and everything with
// it, the starter, clicks on windows in the background) and what goes on to
// the client under the pointer or with the keyboard.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>

#include <linux/input-event-codes.h>
#include <unistd.h>

#include "Apps.hpp"
#include "Desk.hpp"
#include "Hud.hpp"
#include "Server.hpp"
#include "State.hpp"
#include "View.hpp"

namespace raum {

namespace {

std::string picturesDir() {
    const char* home = std::getenv("HOME");
    std::string base = home ? home : "/tmp";
    for (const char* d : {"/Bilder", "/Pictures"}) {
        const std::string p = base + d;
        if (access(p.c_str(), W_OK) == 0) return p;
    }
    return base;
}

} // namespace

bool Server::superHeld() const {
    wlr_keyboard* kb = wlr_seat_get_keyboard(seat);
    return kb && (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_LOGO);
}

// --- Keyboard --------------------------------------------------------------------------

void Server::keyboardModifiers(Keyboard& k) {
    wlr_seat_set_keyboard(seat, k.kb);
    wlr_seat_keyboard_notify_modifiers(seat, &k.kb->modifiers);
}

bool Server::handleBinding(xkb_keysym_t sym, std::uint32_t mods) {
    const bool shift = mods & WLR_MODIFIER_SHIFT;
    View* f = m_focused;
    View* root = f;
    while (root && root->parentView()) root = root->parentView();
    // Digits by their place on the keyboard, whatever the layout shifts them to.
    int digit = -1;
    if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) digit = static_cast<int>(sym - XKB_KEY_1);
    if (digit >= 0) {
        if (digit >= static_cast<int>(desk->rooms.size())) {
            hud->toast("Raum " + std::to_string(digit + 1) + " gibt es noch nicht -- Super+N");
            return true;
        }
        if (shift) desk->moveViewToRoom(root, digit, true);
        else desk->goToRoom(digit);
        return true;
    }
    switch (sym) {
        case XKB_KEY_space:
        case XKB_KEY_a:
            if (hud->launcherOpen()) hud->close();
            else hud->openLauncher();
            return true;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            desk->setLevel(Level::Work);
            spawn(apps->terminal());
            return true;
        case XKB_KEY_Tab:
        case XKB_KEY_ISO_Left_Tab:
            desk->setLevel(Level::Work);
            desk->cycle(shift || sym == XKB_KEY_ISO_Left_Tab ? -1 : 1);
            return true;
        case XKB_KEY_Up:
            desk->zoom(+1);
            return true;
        case XKB_KEY_Down:
            desk->zoom(-1);
            return true;
        case XKB_KEY_Left:
        case XKB_KEY_Right: {
            const int dir = sym == XKB_KEY_Left ? -1 : 1;
            if (shift) {
                const int to = desk->current + dir;
                if (to >= 0 && to < static_cast<int>(desk->rooms.size())) desk->moveViewToRoom(root, to, true);
            } else {
                desk->stepRoom(dir);
            }
            return true;
        }
        case XKB_KEY_n: {
            const int r = desk->newRoom();
            desk->goToRoom(r);
            desk->setLevel(Level::Work);
            hud->openRename(r);
            return true;
        }
        case XKB_KEY_r:
            hud->openRename(desk->current);
            return true;
        case XKB_KEY_p:
            desk->setSplit(desk->rooms[static_cast<std::size_t>(desk->current)].split % 3 + 1);
            return true;
        case XKB_KEY_f:
            if (root) {
                if (shift) root->setFullscreen(!root->fullscreen);
                else root->setMaximized(!root->maximized);
            }
            return true;
        case XKB_KEY_q:
        case XKB_KEY_w:
            if (f) f->close();
            return true;
        case XKB_KEY_F11:
            toggleFullscreen();
            return true;
        case XKB_KEY_h:
        case XKB_KEY_F1:
            hud->toggleHelp();
            return true;
        case XKB_KEY_s: {
            const std::time_t t = std::time(nullptr);
            std::tm lt{};
            localtime_r(&t, &lt);
            char name[64];
            std::strftime(name, sizeof(name), "/raum-%Y%m%d-%H%M%S.png", &lt);
            takeScreenshot(picturesDir() + name);
            hud->toast("Bildschirmfoto gespeichert");
            return true;
        }
        case XKB_KEY_e:
            if (shift) {
                hud->askQuit();
                return true;
            }
            return false;
        case XKB_KEY_BackSpace:
            desk->deleteRoomIfEmpty(desk->current);
            return true;
        case XKB_KEY_Escape:
            desk->setLevel(Level::Work);
            return true;
        default:
            return false;
    }
}

void Server::keyboardKey(Keyboard& k, const wlr_keyboard_key_event& e) {
    const std::uint32_t keycode = e.keycode + 8;
    const bool pressed = e.state == WL_KEYBOARD_KEY_STATE_PRESSED;
    const std::uint32_t mods = wlr_keyboard_get_modifiers(k.kb);
    xkb_state* xs = k.kb->xkb_state;
    const xkb_keysym_t sym = xkb_state_key_get_one_sym(xs, keycode);
    // The key as on the unshifted layer, for bindings with Shift.
    const xkb_keysym_t* base = nullptr;
    const xkb_layout_index_t layout = xkb_state_key_get_layout(xs, keycode);
    const int nb = xkb_keymap_key_get_syms_by_level(k.kb->keymap, keycode, layout, 0, &base);
    const xkb_keysym_t plain = nb > 0 ? xkb_keysym_to_lower(base[0]) : sym;

    // Leaving for another virtual terminal (a real session).
    if (pressed && sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
        if (session) wlr_session_change_vt(session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
        return;
    }

    // Super on its own, pressed and let go: step back to see the room.
    const bool isSuper = plain == XKB_KEY_Super_L || plain == XKB_KEY_Super_R;
    if (isSuper) {
        if (pressed) m_superTap = !(mods & ~WLR_MODIFIER_LOGO);
        else if (m_superTap) {
            m_superTap = false;
            if (!hud->wantsKeyboard()) desk->setLevel(desk->level == Level::Work ? Level::Room : Level::Work);
            return;
        }
    } else if (pressed) {
        m_superTap = false;
    }

    if (hud->wantsKeyboard() && !isSuper) {
        char utf8[16] = {};
        xkb_state_key_get_utf8(xs, keycode, utf8, sizeof(utf8));
        if (!(pressed && (mods & WLR_MODIFIER_LOGO) && handleBinding(plain, mods))) hud->key(sym, pressed, mods, utf8);
        return;
    }

    if (pressed && (mods & WLR_MODIFIER_LOGO) && !isSuper) {
        if (handleBinding(plain, mods)) {
            m_swallowed.push_back(e.keycode);
            return;
        }
    }
    if (!pressed) {
        // The release of a key whose press raum kept goes nowhere either.
        auto it = std::find(m_swallowed.begin(), m_swallowed.end(), e.keycode);
        if (it != m_swallowed.end()) {
            m_swallowed.erase(it);
            return;
        }
    }
    if (pressed && desk->level != Level::Work && sym == XKB_KEY_Escape) {
        desk->setLevel(Level::Work);
        m_swallowed.push_back(e.keycode);
        return;
    }
    wlr_seat_set_keyboard(seat, k.kb);
    wlr_seat_keyboard_notify_key(seat, e.time_msec, e.keycode, e.state);
}

// --- Pointer -----------------------------------------------------------------------------

void Server::onCursorMotion(void* data) {
    auto* e = static_cast<wlr_pointer_motion_event*>(data);
    wlr_cursor_move(cursor, &e->pointer->base, e->delta_x, e->delta_y);
    pointerMoved(e->time_msec);
}

void Server::onCursorMotionAbsolute(void* data) {
    auto* e = static_cast<wlr_pointer_motion_absolute_event*>(data);
    wlr_cursor_warp_absolute(cursor, &e->pointer->base, e->x, e->y);
    pointerMoved(e->time_msec);
}

void Server::onCursorButton(void* data) {
    auto* e = static_cast<wlr_pointer_button_event*>(data);
    pointerButton(e->time_msec, e->button, e->state == WL_POINTER_BUTTON_STATE_PRESSED);
}

void Server::onCursorAxis(void* data) { pointerAxis(*static_cast<wlr_pointer_axis_event*>(data)); }

void Server::onCursorFrame(void*) { wlr_seat_pointer_notify_frame(seat); }

void Server::refreshPointerFocus() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    pointerMoved(static_cast<std::uint32_t>(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
}

void Server::pointerMoved(std::uint32_t time) {
    const double x = cursor->x, y = cursor->y;
    hud->pointerMotion(x, y);
    scheduleFrame();

    switch (m_grab) {
        case Grab::Move:
            if (m_grabView) m_grabView->userOffset = m_grabOffsetStart + glm::vec2(x - m_grabStart.x, y - m_grabStart.y);
            return;
        case Grab::Resize:
            if (m_grabView) {
                const double dx = x - m_grabStart.x, dy = y - m_grabStart.y;
                int w = m_grabW, h = m_grabH;
                if (m_grabEdges & WLR_EDGE_RIGHT) w += static_cast<int>(dx);
                if (m_grabEdges & WLR_EDGE_LEFT) w -= static_cast<int>(dx);
                if (m_grabEdges & WLR_EDGE_BOTTOM) h += static_cast<int>(dy);
                if (m_grabEdges & WLR_EDGE_TOP) h -= static_cast<int>(dy);
                w = std::max(w, 120);
                h = std::max(h, 80);
                // The window is centred: growing it to one side moves it half as
                // far the other way, so the edge under the pointer follows.
                glm::vec2 off = m_grabOffsetStart;
                if (m_grabEdges & WLR_EDGE_RIGHT) off.x += (w - m_grabW) * 0.5f;
                if (m_grabEdges & WLR_EDGE_LEFT) off.x -= (w - m_grabW) * 0.5f;
                if (m_grabEdges & WLR_EDGE_BOTTOM) off.y += (h - m_grabH) * 0.5f;
                if (m_grabEdges & WLR_EDGE_TOP) off.y -= (h - m_grabH) * 0.5f;
                m_grabView->userOffset = off;
                m_grabView->sizedByDesk = false;
                m_grabView->askSize(w, h);
            }
            return;
        case Grab::Hud:
            return;
        case Grab::Desk:
            desk->dragPointer = glm::dvec2(x, y);
            return;
        case Grab::Client:
            if (m_grabView && seat->pointer_state.focused_surface) {
                // Held: the pressed surface keeps the pointer, wherever it goes.
                double mx = 0, my = 0;
                if (desk->pointOnView(m_grabView, x, y, mx, my)) {
                    double sx = mx, sy = my;
                    wlr_surface* s = seat->pointer_state.focused_surface;
                    // Offset of that surface in the window's tree.
                    m_grabView->forEachSurface([&](wlr_surface* t, int ox, int oy) {
                        if (t == s) {
                            sx = mx - ox;
                            sy = my - oy;
                        }
                    });
                    wlr_seat_pointer_notify_motion(seat, time, sx, sy);
                }
            }
            return;
        case Grab::None:
            break;
    }

    if (hud->wantsPointer()) {
        if (seat->pointer_state.focused_surface) wlr_seat_pointer_notify_clear_focus(seat);
        if (m_clientCursor) resetCursorImage();
        desk->hovered = nullptr;
        return;
    }
    const Desk::Hit hit = desk->pick(x, y);
    View* hover = hit.view && !hit.front ? hit.view : nullptr;
    if (hover != desk->hovered) {
        desk->hovered = hover;
        scheduleFrame();
    }
    if (hit.view && hit.front && hit.surface) {
        wlr_seat_pointer_notify_enter(seat, hit.surface, hit.sx, hit.sy);
        wlr_seat_pointer_notify_motion(seat, time, hit.sx, hit.sy);
        m_pointerView = hit.view;
    } else {
        if (seat->pointer_state.focused_surface) wlr_seat_pointer_notify_clear_focus(seat);
        if (m_clientCursor) resetCursorImage();
        m_pointerView = nullptr;
    }
}

void Server::pointerButton(std::uint32_t time, std::uint32_t button, bool pressed) {
    m_buttonsDown = std::max(0, m_buttonsDown + (pressed ? 1 : -1));
    const double x = cursor->x, y = cursor->y;
    scheduleFrame();

    if (!pressed) {
        const Grab g = m_grab;
        if (m_buttonsDown == 0) m_grab = Grab::None;
        switch (g) {
            case Grab::Hud:
                hud->pointerButton(button, false);
                break;
            case Grab::Client:
                wlr_seat_pointer_notify_button(seat, time, button, WL_POINTER_BUTTON_STATE_RELEASED);
                break;
            case Grab::Desk:
                if (desk->dragging) {
                    // Dropped: onto another room it moves there, else it is a click.
                    View* v = desk->dragging;
                    desk->dragging = nullptr;
                    const int over = desk->pickRoom(x, y);
                    const bool moved = glm::length(glm::dvec2(x, y) - m_grabStart) > 12.0;
                    if (moved && over >= 0 && over != v->room) {
                        desk->moveViewToRoom(v, over, false);
                    } else if (!moved) {
                        desk->setLevel(Level::Work);
                        focus(v);
                    }
                }
                break;
            case Grab::Move:
            case Grab::Resize:
                if (m_buttonsDown == 0) m_grabView = nullptr;
                break;
            case Grab::None:
                wlr_seat_pointer_notify_button(seat, time, button, WL_POINTER_BUTTON_STATE_RELEASED);
                break;
        }
        if (m_buttonsDown == 0) refreshPointerFocus();
        return;
    }

    if (m_grab != Grab::None) {
        // A second button during a grab goes where the first one went.
        if (m_grab == Grab::Client)
            wlr_seat_pointer_notify_button(seat, time, button, WL_POINTER_BUTTON_STATE_PRESSED);
        else if (m_grab == Grab::Hud)
            hud->pointerButton(button, true);
        return;
    }
    if (hud->wantsPointer()) {
        m_grab = Grab::Hud;
        hud->pointerButton(button, true);
        return;
    }

    const Desk::Hit hit = desk->pick(x, y);
    if (desk->level != Level::Work) {
        // Stepped back: a click picks a window (or a room), a drag carries one.
        m_grab = Grab::Desk;
        m_grabStart = glm::dvec2(x, y);
        if (hit.view) {
            View* root = hit.view;
            while (View* p = root->parentView()) root = p;
            desk->dragging = desk->level == Level::House ? root : nullptr;
            desk->dragPointer = glm::dvec2(x, y);
            if (!desk->dragging) {
                desk->setLevel(Level::Work);
                focus(root);
                m_grab = Grab::None;
            }
        } else if (desk->level == Level::House) {
            const int r = desk->pickRoom(x, y);
            if (r >= 0) {
                desk->goToRoom(r);
                desk->setLevel(Level::Work);
            }
            m_grab = Grab::None;
        }
        return;
    }

    if (!hit.view) return;
    View* root = hit.view;
    while (View* p = root->parentView()) root = p;
    if (superHeld()) {
        // Super + drag moves a window in its plane, Super + right drag sizes it.
        if (hit.front) {
            m_grabView = root;
            m_grabStart = glm::dvec2(x, y);
            m_grabOffsetStart = root->userOffset;
            if (button == BTN_RIGHT) {
                m_grab = Grab::Resize;
                m_grabEdges = WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM;
                const wlr_box g = root->geometry();
                m_grabW = g.width;
                m_grabH = g.height;
            } else {
                m_grab = Grab::Move;
            }
            wlr_seat_pointer_notify_clear_focus(seat);
            focus(hit.view);
        }
        return;
    }
    if (!hit.front) {
        // A window in the background: the click brings it forward, nothing more.
        focus(root);
        refreshPointerFocus();
        return;
    }
    if (m_focused != hit.view) focus(hit.view);
    m_grab = Grab::Client;
    m_grabView = hit.view;
    wlr_seat_pointer_notify_button(seat, time, button, WL_POINTER_BUTTON_STATE_PRESSED);
}

void Server::pointerAxis(const wlr_pointer_axis_event& e) {
    const bool vertical = e.orientation == WL_POINTER_AXIS_VERTICAL_SCROLL;
    if (hud->pointerAxis(vertical ? 0 : e.delta, vertical ? e.delta : 0)) return;
    if (superHeld() || desk->level != Level::Work) {
        if (!vertical) return;
        // Super + wheel steps back and forth; stepped back, the wheel walks the
        // room's history.
        m_axisAccum += e.delta;
        if (std::abs(m_axisAccum) < 10.0) return;
        const int dir = m_axisAccum > 0 ? 1 : -1;
        m_axisAccum = 0.0;
        if (superHeld()) desk->zoom(dir);
        else desk->dolly(dir > 0 ? -1.0f : 1.0f);
        return;
    }
    wlr_seat_pointer_notify_axis(seat, e.time_msec, e.orientation, e.delta, e.delta_discrete,
                                 e.source, e.relative_direction);
}

// --- Script commands ------------------------------------------------------------------------

namespace {
const wlr_keyboard_impl kScriptKeyboard = {"raum-script-keyboard", nullptr};
}

wlr_keyboard* Server::scriptKeyboard() {
    if (!m_scriptKbReady) {
        wlr_keyboard_init(&m_scriptKb, &kScriptKeyboard, "raum-script");
        keyboards.push_back(std::make_unique<Keyboard>(*this, &m_scriptKb));
        wlr_seat_set_capabilities(seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
        m_scriptKbReady = true;
        if (m_focused) {
            View* f = m_focused;
            m_focused = nullptr;
            focus(f);   // enter again, now with a keyboard
        }
    }
    return &m_scriptKb;
}

namespace {

void pressKey(wlr_keyboard* kb, std::uint32_t evdev, bool down, std::uint32_t& t) {
    wlr_keyboard_key_event e{};
    e.time_msec = t++;
    e.keycode = evdev;
    e.update_state = true;
    e.state = down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
    wlr_keyboard_notify_key(kb, &e);
}

// The key (evdev code) and level that produce `sym` in the keyboard's layout.
bool keyFor(wlr_keyboard* kb, xkb_keysym_t sym, std::uint32_t& evdev, bool& shift) {
    xkb_keymap* map = kb->keymap;
    for (xkb_keycode_t kc = xkb_keymap_min_keycode(map); kc <= xkb_keymap_max_keycode(map); ++kc)
        for (xkb_level_index_t lvl = 0; lvl < 2; ++lvl) {
            const xkb_keysym_t* syms = nullptr;
            const int n = xkb_keymap_key_get_syms_by_level(map, kc, 0, lvl, &syms);
            for (int i = 0; i < n; ++i)
                if (syms[i] == sym) {
                    evdev = kc - 8;
                    shift = lvl == 1;
                    return true;
                }
        }
    return false;
}

} // namespace

void Server::scriptKeys(const std::string& combo) {
    wlr_keyboard* kb = scriptKeyboard();
    std::vector<std::uint32_t> mods;
    std::string key = combo;
    for (;;) {
        const auto plus = key.find('+');
        if (plus == std::string::npos || plus == 0) break;
        const std::string m = key.substr(0, plus);
        key = key.substr(plus + 1);
        if (m == "super") mods.push_back(KEY_LEFTMETA);
        else if (m == "shift") mods.push_back(KEY_LEFTSHIFT);
        else if (m == "ctrl") mods.push_back(KEY_LEFTCTRL);
        else if (m == "alt") mods.push_back(KEY_LEFTALT);
    }
    std::uint32_t t = static_cast<std::uint32_t>(now() * 1000);
    std::uint32_t code = 0;
    bool shift = false;
    const xkb_keysym_t sym = xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
    const bool found = !key.empty() && sym != XKB_KEY_NoSymbol && keyFor(kb, sym, code, shift);
    for (std::uint32_t m : mods) pressKey(kb, m, true, t);
    if (found) {
        pressKey(kb, code, true, t);
        pressKey(kb, code, false, t);
    } else if (!key.empty()) {
        std::fprintf(stderr, "[raum] script: no key %s\n", key.c_str());
    }
    for (auto it = mods.rbegin(); it != mods.rend(); ++it) pressKey(kb, *it, false, t);
}

void Server::scriptType(const std::string& text) {
    wlr_keyboard* kb = scriptKeyboard();
    std::uint32_t t = static_cast<std::uint32_t>(now() * 1000);
    // UTF-8 to code points, each through the layout.
    for (std::size_t i = 0; i < text.size();) {
        std::uint32_t cp = static_cast<unsigned char>(text[i]);
        int len = 1;
        if (cp >= 0xF0) { cp &= 0x07; len = 4; }
        else if (cp >= 0xE0) { cp &= 0x0F; len = 3; }
        else if (cp >= 0xC0) { cp &= 0x1F; len = 2; }
        for (int k = 1; k < len && i + k < text.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        i += len;
        const xkb_keysym_t sym = cp == '\n' ? XKB_KEY_Return : xkb_utf32_to_keysym(cp);
        std::uint32_t code = 0;
        bool shift = false;
        if (!keyFor(kb, sym, code, shift)) continue;
        if (shift) pressKey(kb, KEY_LEFTSHIFT, true, t);
        pressKey(kb, code, true, t);
        pressKey(kb, code, false, t);
        if (shift) pressKey(kb, KEY_LEFTSHIFT, false, t);
    }
}

void Server::scriptCommand(const std::string& cmd, const std::string& arg) {
    if (cmd == "spawn") {
        spawn(arg);
    } else if (cmd == "shot") {
        takeScreenshot(arg);
    } else if (cmd == "fullscreen") {
        toggleFullscreen();
    } else if (cmd == "outshot") {
        pendingOutputShot = arg;
        scheduleFrame();
    } else if (cmd == "quit") {
        quit();
    } else if (cmd == "level") {
        desk->setLevel(arg == "house" ? Level::House : arg == "room" ? Level::Room : Level::Work);
    } else if (cmd == "room") {
        desk->goToRoom(std::atoi(arg.c_str()) - 1);
    } else if (cmd == "newroom") {
        desk->goToRoom(desk->newRoom(arg));
    } else if (cmd == "split") {
        desk->setSplit(std::atoi(arg.c_str()));
    } else if (cmd == "cycle") {
        desk->cycle(arg == "back" ? -1 : 1);
    } else if (cmd == "launcher") {
        hud->openLauncher();
    } else if (cmd == "help") {
        hud->toggleHelp();
    } else if (cmd == "close") {
        hud->close();
    } else if (cmd == "dolly") {
        desk->dolly(static_cast<float>(std::atof(arg.c_str())));
    } else if (cmd == "move") {
        // move <room>: the focused window, staying here
        desk->moveViewToRoom(m_focused, std::atoi(arg.c_str()) - 1, false);
    } else if (cmd == "pointer" || cmd == "click") {
        std::istringstream in(arg);
        double x = 0, y = 0;
        in >> x >> y;
        wlr_cursor_warp(cursor, nullptr, x, y);
        refreshPointerFocus();
        wlr_seat_pointer_notify_frame(seat);
        if (cmd == "click") {
            const std::uint32_t t = static_cast<std::uint32_t>(now() * 1000);
            pointerButton(t, BTN_LEFT, true);
            wlr_seat_pointer_notify_frame(seat);
            pointerButton(t + 1, BTN_LEFT, false);
            wlr_seat_pointer_notify_frame(seat);
        }
    } else if (cmd == "drag") {
        // drag x0 y0 x1 y1: press, move in steps, let go
        std::istringstream in(arg);
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        in >> x0 >> y0 >> x1 >> y1;
        std::uint32_t t = static_cast<std::uint32_t>(now() * 1000);
        wlr_cursor_warp(cursor, nullptr, x0, y0);
        refreshPointerFocus();
        pointerButton(t++, BTN_LEFT, true);
        wlr_seat_pointer_notify_frame(seat);
        for (int i = 1; i <= 12; ++i) {
            wlr_cursor_warp(cursor, nullptr, x0 + (x1 - x0) * i / 12.0, y0 + (y1 - y0) * i / 12.0);
            pointerMoved(t++);
            wlr_seat_pointer_notify_frame(seat);
        }
        pointerButton(t++, BTN_LEFT, false);
        wlr_seat_pointer_notify_frame(seat);
    } else if (cmd == "press" || cmd == "release") {
        const std::uint32_t t = static_cast<std::uint32_t>(now() * 1000);
        pointerButton(t, BTN_LEFT, cmd == "press");
        wlr_seat_pointer_notify_frame(seat);
    } else if (cmd == "focus") {
        // focus <app id or title part>
        for (const auto& v : views)
            if (v->mapped() && (v->appId().find(arg) != std::string::npos ||
                                v->title().find(arg) != std::string::npos)) {
                focus(v.get());
                break;
            }
    } else if (cmd == "toast") {
        hud->toast(arg);
    } else if (cmd == "key") {
        scriptKeys(arg);
    } else if (cmd == "type") {
        // \n in the script is Enter
        std::string t;
        for (std::size_t i = 0; i < arg.size(); ++i) {
            if (arg[i] == '\\' && i + 1 < arg.size() && arg[i + 1] == 'n') {
                t += '\n';
                ++i;
            } else {
                t += arg[i];
            }
        }
        scriptType(t);
    } else {
        std::fprintf(stderr, "[raum] script: unknown command %s\n", cmd.c_str());
    }
    scheduleFrame();
}

} // namespace raum
