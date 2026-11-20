#include "RingMenu.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#include <unistd.h>

#include <glm/glm.hpp>
#include <imgui.h>

#include <fitzel/rhi/Device.hpp>

#include "Apps.hpp"
#include "Desk.hpp"
#include "Gpu.hpp"
#include "HoloIcons.hpp"
#include "Hud.hpp"
#include "Server.hpp"
#include "Sfx.hpp"
#include "State.hpp"
#include "Theme.hpp"

namespace raum {

namespace {

constexpr float kPi = 3.14159265f;
constexpr int kSlots = 18;          // places on the dial
constexpr float kOpenTime = 0.32f, kCloseTime = 0.18f, kLaunchTime = 0.42f;

ImU32 rgba(const glm::vec3& c, float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    return IM_COL32(static_cast<int>(std::clamp(c.r, 0.0f, 1.0f) * 255),
                    static_cast<int>(std::clamp(c.g, 0.0f, 1.0f) * 255),
                    static_cast<int>(std::clamp(c.b, 0.0f, 1.0f) * 255), static_cast<int>(a * 255));
}

ImVec2 polar(float cx, float cy, float r, float a) { return ImVec2(cx + std::cos(a) * r, cy + std::sin(a) * r); }

void centeredText(ImDrawList* dl, ImFont* f, float size, ImVec2 c, ImU32 col, const char* text) {
    const ImVec2 s = f->CalcTextSizeA(size, 1e9f, 0.0f, text);
    dl->AddText(f, size, ImVec2(c.x - s.x * 0.5f, c.y - s.y * 0.5f), col, text);
}

// Shorten to `width` pixels with an ellipsis.
std::string fitText(ImFont* f, float size, const std::string& s, float width) {
    if (f->CalcTextSizeA(size, 1e9f, 0.0f, s.c_str()).x <= width) return s;
    std::string t = s;
    while (!t.empty() && f->CalcTextSizeA(size, 1e9f, 0.0f, (t + "…").c_str()).x > width) {
        t.pop_back();
        while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();
    }
    return t + "…";
}

std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    const std::pair<const char*, const char*> um[] = {
        {"\xC3\xA4", "\xC3\x84"}, {"\xC3\xB6", "\xC3\x96"}, {"\xC3\xBC", "\xC3\x9C"}};
    for (const auto& [lo, up] : um)
        for (std::size_t p = 0; (p = s.find(lo, p)) != std::string::npos; p += 2) s.replace(p, 2, up);
    return s;
}

bool hasCategory(const std::string& cats, const char* c) {
    const std::string needle = std::string(c) + ";";
    return (";" + cats).find(";" + needle) != std::string::npos || cats == c;
}

// The one category a program is listed under: the most specific it names.
int categoryOf(const App& a) {
    const std::string& c = a.categories;
    if (hasCategory(c, "Game")) return 7;
    if (hasCategory(c, "Development") || hasCategory(c, "IDE")) return 6;
    if (hasCategory(c, "Graphics")) return 4;
    if (hasCategory(c, "AudioVideo") || hasCategory(c, "Audio") || hasCategory(c, "Video")) return 5;
    if (hasCategory(c, "Network") || hasCategory(c, "WebBrowser") || hasCategory(c, "Email")) return 2;
    if (hasCategory(c, "Office")) return 3;
    if (hasCategory(c, "System") || hasCategory(c, "Settings") || hasCategory(c, "Monitor") ||
        hasCategory(c, "TerminalEmulator"))
        return 8;
    return 9;
}

const char* kCatNames[] = {"Alle", "Zuletzt", "Internet", "Büro", "Grafik", "Medien",
                           "Entwicklung", "Spiele", "System", "Zubehör"};
const Glyph kCatGlyphs[] = {Glyph::All, Glyph::Recent, Glyph::Internet, Glyph::Office, Glyph::Graphics,
                            Glyph::Media, Glyph::Development, Glyph::Games, Glyph::System,
                            Glyph::Accessories};

struct ActionInfo { const char* label; Glyph glyph; };
const ActionInfo kActions[] = {{"Neuer Raum", Glyph::NewRoom}, {"Bildschirmfoto", Glyph::Screenshot},
                               {"Ton", Glyph::SoundOn}, {"Hilfe", Glyph::Help},
                               {"Beenden", Glyph::Power}};

int wrap(int i, int n) { return ((i % n) + n) % n; }

// Where things were drawn, for the pointer: one frame behind, which is
// what the eye is looking at.
struct Slot { int raw = 0, app = -1; ImVec2 pos; float r = 0; };
std::vector<Slot> g_slots;
struct Seg { float a0 = 0, a1 = 0; };
std::vector<Seg> g_segs;
ImVec2 g_actionPos[5];
float g_actionR = 0;

} // namespace

RingMenu::RingMenu(Server& server) : m_server(server) {}

int RingMenu::slots() const { return kSlots; }

// --- Opening and closing -----------------------------------------------------------------

void RingMenu::open(int room) {
    m_room = room < 0 ? m_server.desk->current : room;
    m_query.clear();
    rebuild();
    // Start where something is: what this room was using, else everything.
    m_cat = (m_cats.size() > 1 && m_cats[1].glyph == static_cast<int>(Glyph::Recent)) ? 1 : 0;
    m_sel = 0;
    m_rot = 0.0f;
    m_hoverCat = m_hoverAction = -1;
    m_onApp = false;
    m_launchApp = -1;
    m_state = State::Opening;
    m_t0 = m_server.desk->now();
    m_server.sfx->play(Sfx::MenuOpen);
    m_server.desk->wave(glm::vec3(m_server.desk->roomX(m_server.desk->current), m_server.desk->floorY(), 300.0f), 0.7f);
    m_server.desk->poke();
    m_server.scheduleFrame();
}

void RingMenu::close() {
    if (m_state == State::Closed || m_state == State::Closing) return;
    if (m_state != State::Launching) m_server.sfx->play(Sfx::MenuClose);
    m_state = State::Closing;
    m_t0 = m_server.desk->now();
    m_server.scheduleFrame();
}

void RingMenu::rebuild() {
    Apps& apps = *m_server.apps;
    Desk& desk = *m_server.desk;
    std::vector<std::vector<int>> by(10);
    for (int i = 0; i < static_cast<int>(apps.all().size()); ++i) {
        by[0].push_back(i);
        by[static_cast<std::size_t>(categoryOf(apps.all()[static_cast<std::size_t>(i)]))].push_back(i);
    }
    // Recent: this room's first, then the other rooms'.
    auto addRecent = [&](const std::vector<std::string>& ids) {
        for (const std::string& id : ids)
            for (int i : by[0])
                if (apps.all()[static_cast<std::size_t>(i)].id == id &&
                    std::find(by[1].begin(), by[1].end(), i) == by[1].end())
                    by[1].push_back(i);
    };
    if (m_room >= 0 && m_room < static_cast<int>(desk.rooms.size()))
        addRecent(desk.rooms[static_cast<std::size_t>(m_room)].recentApps);
    for (const Room& r : desk.rooms) addRecent(r.recentApps);
    // Nothing started yet anywhere: the usual suspects, those that are here.
    m_recentIsDefault = by[1].empty();
    if (m_recentIsDefault)
        addRecent({"foot", "org.gnome.Ptyxis", "firefox", "firefox_firefox", "google-chrome",
                   "org.gnome.Nautilus", "org.gnome.TextEditor", "code_code", "code",
                   "org.gnome.Calculator", "org.gnome.clocks", "org.gnome.SystemMonitor",
                   "org.gnome.Settings"});
    if (by[1].size() > kSlots) by[1].resize(kSlots);
    m_cats.clear();
    for (int c = 0; c < 10; ++c)
        if (c == 0 || !by[static_cast<std::size_t>(c)].empty())
            m_cats.push_back({c == 1 && m_recentIsDefault ? "Empfohlen" : kCatNames[c], static_cast<int>(kCatGlyphs[c]),
                              by[static_cast<std::size_t>(c)]});
    m_found = m_query.empty() ? std::vector<int>{} : apps.search(m_query);
}

const std::vector<int>& RingMenu::list() const {
    if (!m_query.empty()) return m_found;
    return m_cats[static_cast<std::size_t>(std::clamp(m_cat, 0, static_cast<int>(m_cats.size()) - 1))].apps;
}

void RingMenu::select(int index, bool sound) {
    const int n = static_cast<int>(list().size());
    if (n == 0) return;
    if (n <= kSlots) index = std::clamp(index, 0, n - 1);
    if (index != m_sel && sound) m_server.sfx->play(Sfx::Tick, 0.8f);
    m_sel = index;
    m_server.scheduleFrame();
}

void RingMenu::turn(int steps) { select(m_sel + steps, true); }

// --- Starting things -------------------------------------------------------------------------

void RingMenu::launchSelected() {
    const auto& l = list();
    if (l.empty()) {
        m_server.sfx->play(Sfx::Error);
        return;
    }
    launch(l[static_cast<std::size_t>(wrap(m_sel, static_cast<int>(l.size())))]);
}

void RingMenu::launch(int app) {
    m_launchApp = app;
    m_state = State::Launching;
    m_t0 = m_server.desk->now();
    m_server.sfx->play(Sfx::Launch);
    m_server.hud->launch(app, m_room);
    m_server.scheduleFrame();
}

void RingMenu::runAction(int a) {
    Hud& hud = *m_server.hud;
    Desk& desk = *m_server.desk;
    m_state = State::Closed;   // the action's own card or sound follows straight away
    switch (a) {
        case NewRoom: {
            const int r = desk.newRoom();
            desk.goToRoom(r);
            desk.setLevel(Level::Work);
            hud.openRename(r);
            break;
        }
        case Screenshot: {
            const std::time_t t = std::time(nullptr);
            std::tm lt{};
            localtime_r(&t, &lt);
            char name[64];
            std::strftime(name, sizeof(name), "/raum-%Y%m%d-%H%M%S.png", &lt);
            const char* home = std::getenv("HOME");
            std::string dir = home ? std::string(home) + "/Bilder" : "/tmp";
            if (home && access(dir.c_str(), W_OK) != 0) dir = std::string(home) + "/Pictures";
            m_server.takeScreenshot(dir + name);
            m_server.sfx->play(Sfx::Shutter);
            hud.toast("Bildschirmfoto gespeichert");
            break;
        }
        case Sound:
            m_server.toggleSound();
            m_state = State::Open;   // stays: you see the switch change
            break;
        case Help: hud.toggleHelp(); break;
        case Quit: hud.askQuit(); break;
        default: break;
    }
    m_server.scheduleFrame();
}

// --- Input ----------------------------------------------------------------------------------

bool RingMenu::key(xkb_keysym_t sym, bool pressed, std::uint32_t mods, const char* utf8) {
    if (!isOpen()) return false;
    if (!pressed) return true;
    m_server.desk->poke();
    m_server.scheduleFrame();
    if (m_state == State::Launching || m_state == State::Closing) return true;
    switch (sym) {
        case XKB_KEY_Escape:
            if (!m_query.empty()) {
                m_query.clear();
                rebuild();
                select(0, false);
                m_server.sfx->play(Sfx::Tick);
            } else {
                close();
            }
            return true;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            launchSelected();
            return true;
        case XKB_KEY_Left: turn(-1); return true;
        case XKB_KEY_Right: turn(1); return true;
        case XKB_KEY_Up:
        case XKB_KEY_Down:
        case XKB_KEY_Tab:
        case XKB_KEY_ISO_Left_Tab: {
            if (!m_query.empty()) return true;
            const int dir = (sym == XKB_KEY_Up || sym == XKB_KEY_ISO_Left_Tab) ? -1 : 1;
            m_cat = wrap(m_cat + dir, static_cast<int>(m_cats.size()));
            m_sel = 0;
            m_rot = 0.0f;
            m_server.sfx->play(Sfx::Tick);
            return true;
        }
        case XKB_KEY_BackSpace:
            if (!m_query.empty()) {
                std::size_t n = m_query.size();
                while (n > 0 && (static_cast<unsigned char>(m_query[n - 1]) & 0xC0) == 0x80) --n;
                m_query.resize(n > 0 ? n - 1 : 0);
                if ((mods & WLR_MODIFIER_CTRL) != 0) m_query.clear();
                rebuild();
                m_sel = 0;
                m_rot = 0.0f;
            }
            return true;
        default:
            break;
    }
    if (utf8 && static_cast<unsigned char>(utf8[0]) >= 0x20 && utf8[0] != 0x7f &&
        !(mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT))) {
        m_query += utf8;
        rebuild();
        m_sel = 0;
        m_rot = 0.0f;
    }
    return true;
}

void RingMenu::pointerMotion(double x, double y) {
    m_mx = x;
    m_my = y;
    if (!isOpen()) return;
    int app = 0, cat = -1, action = -1;
    bool onApp = false;
    for (const Slot& s : g_slots) {
        const float dx = static_cast<float>(x) - s.pos.x, dy = static_cast<float>(y) - s.pos.y;
        if (dx * dx + dy * dy <= s.r * s.r) {
            app = s.raw;
            onApp = true;
        }
    }
    const float dx = static_cast<float>(x) - m_cx, dy = static_cast<float>(y) - m_cy;
    const float r = std::sqrt(dx * dx + dy * dy) / m_u;
    if (r >= 132.0f && r <= 184.0f && m_query.empty()) {
        float a = std::atan2(dy, dx);
        for (std::size_t i = 0; i < g_segs.size(); ++i) {
            float b = a;
            while (b < g_segs[i].a0) b += 2.0f * kPi;
            if (b <= g_segs[i].a1) cat = static_cast<int>(i);
        }
    }
    for (int i = 0; i < ActionCount; ++i) {
        const float ax = static_cast<float>(x) - g_actionPos[i].x, ay = static_cast<float>(y) - g_actionPos[i].y;
        if (ax * ax + ay * ay <= g_actionR * g_actionR) action = i;
    }
    m_hoverCore = r < 120.0f;
    m_onApp = onApp;
    m_hoverApp = app;
    m_hoverCat = cat;
    m_hoverAction = action;
    m_server.scheduleFrame();
}

bool RingMenu::pointerButton(std::uint32_t button, bool pressed) {
    if (!isOpen()) return false;
    if (!pressed || m_state == State::Launching || m_state == State::Closing) return true;
    if (button == 0x111) {   // BTN_RIGHT: away
        close();
        return true;
    }
    if (m_onApp) {
        const auto& l = list();
        if (!l.empty()) {
            const int idx = wrap(m_hoverApp, static_cast<int>(l.size()));
            m_sel = m_hoverApp;
            launch(l[static_cast<std::size_t>(idx)]);
        }
        return true;
    }
    if (m_hoverCat >= 0) {
        m_cat = m_hoverCat;
        m_sel = 0;
        m_rot = 0.0f;
        m_server.sfx->play(Sfx::Tick);
        return true;
    }
    if (m_hoverAction >= 0) {
        runAction(m_hoverAction);
        return true;
    }
    if (m_hoverCore) {
        launchSelected();
        return true;
    }
    // A click beside the ring puts it away.
    const float dx = static_cast<float>(m_mx) - m_cx, dy = static_cast<float>(m_my) - m_cy;
    if (std::sqrt(dx * dx + dy * dy) > 360.0f * m_u) close();
    return true;
}

bool RingMenu::pointerAxis(double dy) {
    if (!isOpen()) return false;
    static double acc = 0;
    acc += dy;
    if (std::abs(acc) >= 10.0) {
        turn(acc > 0 ? 1 : -1);
        acc = 0;
    }
    return true;
}

// --- Drawing ----------------------------------------------------------------------------------

void RingMenu::draw(ImDrawList* dl, const Fonts& f, int width, int height, double now) {
    if (!isOpen()) return;
    const double dt = std::clamp(now - m_now, 0.0, 0.1);
    m_now = now;
    const float age = static_cast<float>(now - m_t0);
    float e = 1.0f;
    switch (m_state) {
        case State::Opening:
            e = theme::easeOut(age / kOpenTime);
            if (age >= kOpenTime) m_state = State::Open;
            break;
        case State::Closing:
            e = 1.0f - theme::easeOut(age / kCloseTime);
            if (age >= kCloseTime) {
                m_state = State::Closed;
                return;
            }
            break;
        case State::Launching:
            e = 1.0f - theme::easeInOut((age - 0.16f) / (kLaunchTime - 0.16f));
            if (age >= kLaunchTime) {
                m_state = State::Closed;
                return;
            }
            break;
        default: break;
    }

    const Desk& desk = *m_server.desk;
    Apps& apps = *m_server.apps;
    fitzel::rhi::Device& dev = m_server.gpu->device();
    const glm::vec3 acc = desk.rooms[static_cast<std::size_t>(std::clamp(m_room, 0, static_cast<int>(desk.rooms.size()) - 1))].accent;
    const glm::vec3 holo = theme::kHolo;
    const float u = std::max(0.75f, height / 1080.0f);
    const float cx = width * 0.5f, cy = height * 0.5f + 12.0f * u;
    m_cx = cx;
    m_cy = cy;
    m_u = u;
    const float spin = static_cast<float>(std::fmod(now, 600.0));

    // The room behind goes dark, a glow behind the ring.
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(static_cast<float>(width), static_cast<float>(height)),
                      IM_COL32(1, 5, 10, static_cast<int>(175 * e)));
    for (int i = 6; i >= 1; --i)
        dl->AddCircleFilled(ImVec2(cx, cy), (60.0f + i * 55.0f) * u, rgba(theme::kHoloDeep, 0.035f * e), 64);

    // Decorative rings: a ruler that turns, one the other way, arcs.
    const float sweep = 2.0f * kPi * e;
    auto arc = [&](float r, float a0, float a1, ImU32 col, float t) {
        dl->PathArcTo(ImVec2(cx, cy), r, a0, a1, 96);
        dl->PathStroke(col, ImDrawFlags_None, t);
    };
    arc(205.0f * u, -kPi / 2, -kPi / 2 + sweep, rgba(holo, 0.35f * e), 1.0f);
    arc(345.0f * u, -kPi / 2, -kPi / 2 + sweep, rgba(holo, 0.25f * e), 1.0f);
    for (int i = 0; i < 72; ++i) {
        const float a = -kPi / 2 + i * (2 * kPi / 72) + spin * 0.05f;
        if (std::fmod(a + kPi / 2 + 4 * kPi, 2 * kPi) > sweep) continue;
        const float len = (i % 6 == 0 ? 10.0f : 4.0f) * u;
        dl->AddLine(polar(cx, cy, 345.0f * u, a), polar(cx, cy, 345.0f * u + len, a), rgba(holo, 0.45f * e), 1.0f);
    }
    for (int i = 0; i < 36; ++i) {
        const float a = -kPi / 2 + i * (2 * kPi / 36) - spin * 0.08f;
        dl->AddLine(polar(cx, cy, 205.0f * u - 4 * u, a), polar(cx, cy, 205.0f * u + 4 * u, a), rgba(holo, 0.3f * e), 1.0f);
    }
    for (int k = 0; k < 3; ++k) {
        const float a0 = spin * (0.3f + 0.1f * k) + k * 2.1f;
        arc((362.0f + 8.0f * k) * u, a0, a0 + 0.6f + 0.2f * k, rgba(k == 1 ? acc : holo, 0.5f * e), 2.0f);
    }

    // --- Categories --------------------------------------------------------------------------
    const int nc = static_cast<int>(m_cats.size());
    g_segs.assign(static_cast<std::size_t>(nc), Seg{});
    const float segSpan = 2 * kPi / nc;
    const float gap = 0.035f;
    for (int i = 0; i < nc; ++i) {
        const float a0 = -kPi / 2 - segSpan * 0.5f + i * segSpan + gap;
        const float a1 = a0 + segSpan - 2 * gap;
        g_segs[static_cast<std::size_t>(i)] = Seg{a0, a1};
        if (std::fmod(a0 + kPi / 2 + segSpan * 0.5f, 2 * kPi) > sweep + 0.01f) continue;
        const bool sel = i == m_cat && m_query.empty();
        const bool hov = i == m_hoverCat;
        const float rm = 157.0f * u;
        dl->PathArcTo(ImVec2(cx, cy), rm, a0, a1, 24);
        dl->PathStroke(sel ? rgba(acc, 0.32f * e) : rgba(theme::kHoloDeep, (hov ? 0.45f : 0.22f) * e),
                       ImDrawFlags_None, 40.0f * u);
        arc(178.0f * u, a0, a1, rgba(sel ? acc : holo, (sel ? 0.9f : 0.35f) * e), sel ? 2.0f : 1.0f);
        arc(136.0f * u, a0, a1, rgba(holo, 0.25f * e), 1.0f);
        const float am = (a0 + a1) * 0.5f;
        drawGlyph(dl, static_cast<Glyph>(m_cats[static_cast<std::size_t>(i)].glyph), polar(cx, cy, rm, am), 10.0f * u,
                  rgba(sel ? theme::kText : holo, (sel || hov ? 1.0f : 0.7f) * e), 1.4f);
    }
    // The hovered or selected category's name, outside the segment.
    if (m_query.empty() && nc > 0) {
        const int show = m_hoverCat >= 0 ? m_hoverCat : m_cat;
        const Seg& s = g_segs[static_cast<std::size_t>(show)];
        const float am = (s.a0 + s.a1) * 0.5f;
        const std::string name = upper(m_cats[static_cast<std::size_t>(show)].name);
        const ImVec2 p = polar(cx, cy, 214.0f * u, am);
        const float tw = spacedWidth(f.display, 13.0f * u, name.c_str(), 2.0f * u);
        spacedText(dl, f.display, 13.0f * u, ImVec2(p.x - tw * 0.5f, p.y - 7.0f * u),
                   rgba(m_hoverCat >= 0 ? holo : acc, e), name.c_str(), 2.0f * u);
    }

    // --- The dial of programs ------------------------------------------------------------------
    const auto& l = list();
    const int n = static_cast<int>(l.size());
    const bool cyclic = n > kSlots;
    m_rot += (static_cast<float>(m_sel) - m_rot) * (1.0f - std::exp(-16.0f * static_cast<float>(dt)));
    if (std::abs(m_rot - m_sel) < 0.002f) m_rot = static_cast<float>(m_sel);
    g_slots.clear();
    const float step = 2 * kPi / kSlots;
    const float Rapps = 285.0f * u;
    int lo = cyclic ? static_cast<int>(std::floor(m_rot)) - 8 : 0;
    int hi = cyclic ? static_cast<int>(std::floor(m_rot)) + 9 : n - 1;
    for (int raw = lo; raw <= hi; ++raw) {
        const float off = raw - m_rot;
        if (std::abs(off) > kSlots * 0.5f) continue;
        const int idx = wrap(raw, std::max(1, n));
        App& app = const_cast<App&>(apps.all()[static_cast<std::size_t>(l[static_cast<std::size_t>(idx)])]);
        const float a = -kPi / 2 + off * step;
        // Out from the core, one after the other.
        const float delay = 0.06f + 0.014f * std::abs(off);
        float fly = m_state == State::Opening ? theme::easeBack((age - delay) / 0.24f) : e;
        if (m_state == State::Launching && raw == m_sel) fly = 1.0f;
        const float fade = 1.0f - std::clamp((std::abs(off) - 6.5f) / 2.5f, 0.0f, 1.0f) * 0.85f;
        const bool sel = raw == m_sel;
        const bool hov = m_onApp && raw == m_hoverApp;
        float r = (sel ? 34.0f : 27.0f) * u * (hov && !sel ? 1.12f : 1.0f);
        ImVec2 pos = polar(cx, cy, 118.0f * u + (Rapps - 118.0f * u) * std::max(fly, 0.0f), a);
        float alpha = std::clamp(fly, 0.0f, 1.0f) * fade;
        if (m_state == State::Launching) {
            if (raw == m_sel) {
                // The started program flies into the core and flares.
                const float k = theme::easeInOut(age / 0.22f);
                pos = ImVec2(pos.x + (cx - pos.x) * k, pos.y + (cy - pos.y) * k);
                r *= 1.0f + 0.9f * k;
                alpha = 1.0f - theme::easeInOut((age - 0.25f) / 0.15f);
                dl->AddCircle(pos, r * (1.2f + k), rgba(holo, (1.0f - k) * 0.9f), 48, 2.0f);
            } else {
                alpha *= e;
            }
        }
        if (alpha <= 0.01f) continue;
        g_slots.push_back(Slot{raw, l[static_cast<std::size_t>(idx)], pos, r * 1.15f});
        // The socket: a hexagon of light; the selected one lit.
        if (sel) {
            hexagon(dl, pos, r * 1.45f, rgba(acc, 0.20f * alpha), 0, true);
            hexagon(dl, pos, r * 1.45f, rgba(acc, 0.95f * alpha), 2.0f);
        } else {
            hexagon(dl, pos, r * 1.35f, rgba(theme::kPanel, 0.75f * alpha), 0, true);
            hexagon(dl, pos, r * 1.35f, rgba(holo, (hov ? 0.9f : 0.4f) * alpha), 1.2f);
        }
        if (fitzel::Texture* t = apps.icon(app)) {
            dl->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(dev.imguiTextureId(t->id()))),
                                ImVec2(pos.x - r * 0.8f, pos.y - r * 0.8f), ImVec2(pos.x + r * 0.8f, pos.y + r * 0.8f),
                                ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, static_cast<int>(255 * alpha)),
                                r * 0.25f);
        } else {
            const char letter[2] = {app.name.empty() ? '?' : app.name[0], 0};
            centeredText(dl, f.display, r * 1.1f, pos, rgba(holo, alpha), letter);
        }
        if (hov && !sel) {
            const std::string nm = fitText(f.bold, 13.0f * u, app.name, 160.0f * u);
            const ImVec2 tp = polar(cx, cy, Rapps + 52.0f * u, a);
            centeredText(dl, f.bold, 13.0f * u, tp, rgba(theme::kText, alpha), nm.c_str());
        }
    }
    // The pointer at the top: the dial's mark.
    {
        const ImVec2 top(cx, cy - Rapps - 52.0f * u);
        dl->AddTriangleFilled(ImVec2(top.x - 7 * u, top.y), ImVec2(top.x + 7 * u, top.y), ImVec2(top.x, top.y + 9 * u),
                              rgba(acc, e));
    }

    // --- The core ---------------------------------------------------------------------------
    const float rc = 118.0f * u * (0.6f + 0.4f * e);
    dl->AddCircleFilled(ImVec2(cx, cy), rc, rgba(theme::kPanel, 0.92f * e), 96);
    dl->AddCircle(ImVec2(cx, cy), rc, rgba(holo, 0.75f * e), 96, 1.5f);
    dl->AddCircle(ImVec2(cx, cy), rc - 6 * u, rgba(holo, 0.18f * e), 96, 1.0f);
    // The search line at the top of the core.
    {
        const bool blink = std::fmod(now, 1.0) < 0.55;
        std::string q = m_query.empty() ? std::string() : "› " + m_query + (blink ? "_" : " ");
        if (m_query.empty()) {
            drawGlyph(dl, Glyph::Search, ImVec2(cx - 52 * u, cy - 74 * u), 6.0f * u, rgba(theme::kDim, 0.8f * e), 1.2f);
            dl->AddText(f.mono, 11.0f * u, ImVec2(cx - 42 * u, cy - 81 * u), rgba(theme::kDim, 0.8f * e), "TIPPEN: SUCHEN");
        } else {
            q = fitText(f.mono, 14.0f * u, q, 180.0f * u);
            centeredText(dl, f.mono, 14.0f * u, ImVec2(cx, cy - 74 * u),
                         rgba(m_found.empty() ? theme::kWarn : holo, e), q.c_str());
        }
    }
    if (n > 0) {
        App& app = const_cast<App&>(apps.all()[static_cast<std::size_t>(l[static_cast<std::size_t>(wrap(m_sel, n))])]);
        const float ir = 26.0f * u;
        const ImVec2 ic(cx, cy - 22 * u);
        if (fitzel::Texture* t = apps.icon(app))
            dl->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(dev.imguiTextureId(t->id()))),
                                ImVec2(ic.x - ir, ic.y - ir), ImVec2(ic.x + ir, ic.y + ir), ImVec2(0, 0), ImVec2(1, 1),
                                IM_COL32(255, 255, 255, static_cast<int>(255 * e)), ir * 0.25f);
        else
            hexagon(dl, ic, ir, rgba(holo, e), 1.5f);
        const std::string name = fitText(f.bold, 17.0f * u, app.name, 200.0f * u);
        centeredText(dl, f.bold, 17.0f * u, ImVec2(cx, cy + 20 * u), rgba(theme::kText, e), name.c_str());
        std::string sub = !app.comment.empty() ? app.comment : (app.generic.size() > 1 ? app.generic.substr(1) : app.id);
        sub = fitText(f.text, 12.0f * u, sub, 196.0f * u);
        centeredText(dl, f.text, 12.0f * u, ImVec2(cx, cy + 41 * u), rgba(theme::kDim, e), sub.c_str());
        const std::string go = "ENTER  " + upper(desk.rooms[static_cast<std::size_t>(std::clamp(m_room, 0, static_cast<int>(desk.rooms.size()) - 1))].name);
        const std::string goFit = fitText(f.mono, 11.0f * u, go, 190.0f * u);
        centeredText(dl, f.mono, 11.0f * u, ImVec2(cx, cy + 66 * u), rgba(acc, 0.9f * e), goFit.c_str());
    } else {
        centeredText(dl, f.bold, 15.0f * u, ImVec2(cx, cy + 4 * u), rgba(theme::kWarn, e), "Nichts gefunden");
    }
    // Counter, like an instrument.
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%02d / %02d", n ? wrap(m_sel, n) + 1 : 0, n);
        centeredText(dl, f.mono, 10.0f * u, ImVec2(cx, cy + 96 * u), rgba(theme::kDim, 0.8f * e), buf);
    }

    // --- raum's own actions, under the ring ------------------------------------------------------
    g_actionR = 24.0f * u;
    const float ay = cy + 400.0f * u;
    for (int i = 0; i < ActionCount; ++i) {
        const ImVec2 p(cx + (i - 2) * 92.0f * u, ay);
        g_actionPos[i] = p;
        const float d = std::clamp((age - 0.12f - 0.03f * i) / 0.2f, 0.0f, 1.0f);
        const float a = (m_state == State::Opening ? d : 1.0f) * e;
        const bool hov = i == m_hoverAction;
        hexagon(dl, p, 22.0f * u, rgba(theme::kPanel, 0.85f * a), 0, true);
        hexagon(dl, p, 22.0f * u, rgba(i == Quit ? theme::kWarn : holo, (hov ? 1.0f : 0.45f) * a), hov ? 2.0f : 1.2f);
        Glyph g = kActions[i].glyph;
        if (i == Sound && m_server.sfx->muted()) g = Glyph::SoundOff;
        drawGlyph(dl, g, p, 10.0f * u, rgba(i == Quit ? theme::kWarn : (hov ? theme::kText : holo), a), 1.5f);
        const std::string lbl = upper(kActions[i].label);
        const float tw = spacedWidth(f.display, 10.5f * u, lbl.c_str(), 1.5f * u);
        spacedText(dl, f.display, 10.5f * u, ImVec2(p.x - tw * 0.5f, p.y + 28.0f * u),
                   rgba(hov ? theme::kText : theme::kDim, a), lbl.c_str(), 1.5f * u);
    }
    // Hints at the bottom.
    const char* hint = "← →  DREHEN     ↑ ↓  KATEGORIE     ENTER  STARTEN     ESC  ZURÜCK";
    const float hw = spacedWidth(f.display, 12.0f * u, hint, 1.5f * u);
    spacedText(dl, f.display, 12.0f * u, ImVec2(cx - hw * 0.5f, ay + 62.0f * u), rgba(theme::kDim, 0.75f * e), hint, 1.5f * u);
}

} // namespace raum
