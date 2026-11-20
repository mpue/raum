#include "Hud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <glm/glm.hpp>
#include <imgui.h>

#include <fitzel/rhi/Device.hpp>

#include "Apps.hpp"
#include "Desk.hpp"
#include "Gpu.hpp"
#include "HoloIcons.hpp"
#include "Server.hpp"
#include "Sfx.hpp"
#include "State.hpp"
#include "Theme.hpp"
#include "View.hpp"

namespace raum {

namespace {

constexpr float kPi = 3.14159265f;

ImU32 rgba(const glm::vec3& c, float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    return IM_COL32(static_cast<int>(std::clamp(c.r, 0.0f, 1.0f) * 255),
                    static_cast<int>(std::clamp(c.g, 0.0f, 1.0f) * 255),
                    static_cast<int>(std::clamp(c.b, 0.0f, 1.0f) * 255), static_cast<int>(a * 255));
}

ImVec4 vec4(const glm::vec3& c, float a) { return ImVec4(c.r, c.g, c.b, a); }

const char* firstFont(std::initializer_list<const char*> paths) {
    std::error_code ec;
    for (const char* p : paths)
        if (p && std::filesystem::exists(p, ec)) return p;
    return nullptr;
}

std::string ago(double seconds) {
    if (seconds < 50) return "JETZT";
    const int min = static_cast<int>(seconds / 60.0 + 0.5);
    if (min < 60) return "-" + std::to_string(std::max(1, min)) + " MIN";
    const int h = static_cast<int>(seconds / 3600.0 + 0.5);
    if (h < 24) return "-" + std::to_string(h) + " STD";
    const int d = static_cast<int>(seconds / 86400.0 + 0.5);
    return "-" + std::to_string(d) + (d == 1 ? " TAG" : " TAGE");
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

std::string fit(ImFont* f, float size, const std::string& s, float width) {
    if (f->CalcTextSizeA(size, 1e9f, 0.0f, s.c_str()).x <= width) return s;
    std::string t = s;
    while (!t.empty() && f->CalcTextSizeA(size, 1e9f, 0.0f, (t + "…").c_str()).x > width) {
        t.pop_back();
        while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();
    }
    return t + "…";
}

ImVec2 textSize(ImFont* f, float size, const char* s) { return f->CalcTextSizeA(size, 1e9f, 0.0f, s); }

ImGuiKey imguiKey(xkb_keysym_t sym) {
    switch (sym) {
        case XKB_KEY_Return: case XKB_KEY_KP_Enter: return ImGuiKey_Enter;
        case XKB_KEY_Escape: return ImGuiKey_Escape;
        case XKB_KEY_BackSpace: return ImGuiKey_Backspace;
        case XKB_KEY_Delete: return ImGuiKey_Delete;
        case XKB_KEY_Left: return ImGuiKey_LeftArrow;
        case XKB_KEY_Right: return ImGuiKey_RightArrow;
        case XKB_KEY_Up: return ImGuiKey_UpArrow;
        case XKB_KEY_Down: return ImGuiKey_DownArrow;
        case XKB_KEY_Home: return ImGuiKey_Home;
        case XKB_KEY_End: return ImGuiKey_End;
        case XKB_KEY_Tab: return ImGuiKey_Tab;
        default: break;
    }
    if (sym >= XKB_KEY_a && sym <= XKB_KEY_z) return static_cast<ImGuiKey>(ImGuiKey_A + (sym - XKB_KEY_a));
    if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) return static_cast<ImGuiKey>(ImGuiKey_A + (sym - XKB_KEY_A));
    return ImGuiKey_None;
}

const char* kWeekdays[] = {"SO", "MO", "DI", "MI", "DO", "FR", "SA"};
const char* kLevels[] = {"ARBEIT", "RAUM", "HAUS"};

// A chip of the strip: a parallelogram, its sides leaning like the HUD's lines.
void chip(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, ImU32 line) {
    const float s = (b.y - a.y) * 0.35f;
    const ImVec2 pts[4] = {ImVec2(a.x + s, a.y), ImVec2(b.x + s, a.y), ImVec2(b.x - s, b.y), ImVec2(a.x - s, b.y)};
    if ((fill >> IM_COL32_A_SHIFT) & 0xFF) dl->AddConvexPolyFilled(pts, 4, fill);
    if ((line >> IM_COL32_A_SHIFT) & 0xFF) dl->AddPolyline(pts, 4, line, ImDrawFlags_Closed, 1.0f);
}

} // namespace

// --- Setting up ---------------------------------------------------------------------------

Hud::Hud(Server& server) : m_server(server), m_ring(server) {
    IMGUI_CHECKVERSION();
    m_ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Every queued key and character in the next frame, not one per frame: a
    // name typed quickly and confirmed with Enter has to be in the field by
    // the time Enter is acted on.
    io.ConfigInputTrickleEventQueue = false;
    io.BackendPlatformName = "raum";
    // Body text Noto Sans, headings the DIN 1451 that fitzel's signs use (an
    // engineering face reads as instrument lettering in capitals), readouts
    // Noto Sans Mono. DejaVu fills in what a face lacks (arrows).
    const char* regular = firstFont({"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                                     "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"});
    const char* bold = firstFont({"/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
                                  "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"});
    const std::string din = std::string(RAUM_FITZEL_DIR) + "/sandbox/tools/fonts/din1451alt.ttf";
    const char* display = firstFont({din.c_str(), bold});
    const char* mono = firstFont({"/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
                                  "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"});
    const char* fallback = firstFont({"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"});
    const char* fallbackBold = firstFont({"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"});
    auto withFallback = [&](const char* main, const char* extra) -> ImFont* {
        if (!main) return nullptr;
        ImFont* f = io.Fonts->AddFontFromFileTTF(main, 16.0f);
        if (f && extra) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(extra, 16.0f, &cfg);
        }
        return f;
    };
    m_fonts.text = withFallback(regular, fallback);
    if (!m_fonts.text) m_fonts.text = io.Fonts->AddFontDefault();
    m_fonts.bold = withFallback(bold, fallbackBold);
    if (!m_fonts.bold) m_fonts.bold = m_fonts.text;
    m_fonts.display = withFallback(display, fallbackBold);
    if (!m_fonts.display) m_fonts.display = m_fonts.bold;
    m_fonts.mono = withFallback(mono, firstFont({"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"}));
    if (!m_fonts.mono) m_fonts.mono = m_fonts.text;

    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 0.0f;
    st.FrameRounding = 0.0f;
    st.PopupRounding = 0.0f;
    st.WindowBorderSize = 0.0f;
    st.FrameBorderSize = 1.0f;
    st.WindowPadding = ImVec2(26, 22);
    st.FramePadding = ImVec2(12, 8);
    st.ItemSpacing = ImVec2(10, 8);
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_Text] = vec4(theme::kText, 1.0f);
    st.Colors[ImGuiCol_TextDisabled] = vec4(theme::kDim, 1.0f);
    st.Colors[ImGuiCol_Border] = vec4(theme::kHolo, 0.45f);
    st.Colors[ImGuiCol_FrameBg] = vec4(theme::kHoloDeep, 0.25f);
    st.Colors[ImGuiCol_FrameBgHovered] = vec4(theme::kHoloDeep, 0.35f);
    st.Colors[ImGuiCol_FrameBgActive] = vec4(theme::kHoloDeep, 0.45f);
    st.Colors[ImGuiCol_TextSelectedBg] = vec4(theme::kHolo, 0.35f);
    st.Colors[ImGuiCol_NavCursor] = vec4(theme::kHolo, 0.0f);
    st.Colors[ImGuiCol_Button] = vec4(theme::kHoloDeep, 0.25f);
    st.Colors[ImGuiCol_ButtonHovered] = vec4(theme::kHolo, 0.30f);
    st.Colors[ImGuiCol_ButtonActive] = vec4(theme::kHolo, 0.45f);

    m_server.gpu->device().imguiInit(nullptr);
}

Hud::~Hud() {
    if (m_ctx) {
        ImGui::SetCurrentContext(m_ctx);
        m_server.gpu->makeCurrent();
        m_server.gpu->device().imguiShutdown();
        ImGui::DestroyContext(m_ctx);
    }
}

bool Hud::animating() const {
    return !m_toasts.empty() || m_ring.animating() || m_mode == Mode::Rename ||
           (m_mode != Mode::None && m_time - m_modeSince < 0.4);
}

// --- Input ----------------------------------------------------------------------------------

void Hud::pointerMotion(double x, double y) {
    ImGui::SetCurrentContext(m_ctx);
    ImGui::GetIO().AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
    m_ring.pointerMotion(x, y);
}

bool Hud::wantsPointer() const {
    if (m_ring.isOpen() || m_mode != Mode::None) return true;
    ImGui::SetCurrentContext(m_ctx);
    return ImGui::GetIO().WantCaptureMouse;
}

bool Hud::wantsKeyboard() const { return m_ring.isOpen() || m_mode != Mode::None; }

bool Hud::pointerButton(std::uint32_t button, bool pressed) {
    if (m_ring.isOpen()) {
        m_ring.pointerButton(button, pressed);
        return true;
    }
    if (!wantsPointer() && pressed) return false;
    ImGui::SetCurrentContext(m_ctx);
    int b = -1;
    if (button == 0x110) b = 0;        // BTN_LEFT
    else if (button == 0x111) b = 1;   // BTN_RIGHT
    else if (button == 0x112) b = 2;   // BTN_MIDDLE
    if (b >= 0) ImGui::GetIO().AddMouseButtonEvent(b, pressed);
    m_hadInput = true;
    m_server.scheduleFrame();
    return true;
}

bool Hud::pointerAxis(double dx, double dy) {
    if (m_ring.isOpen()) return m_ring.pointerAxis(dy);
    if (!wantsPointer()) return false;
    ImGui::SetCurrentContext(m_ctx);
    ImGui::GetIO().AddMouseWheelEvent(static_cast<float>(-dx / 15.0), static_cast<float>(-dy / 15.0));
    m_server.scheduleFrame();
    return true;
}

bool Hud::key(xkb_keysym_t sym, bool pressed, std::uint32_t mods, const char* utf8) {
    if (m_ring.isOpen()) return m_ring.key(sym, pressed, mods, utf8);
    if (!wantsKeyboard()) return false;
    ImGui::SetCurrentContext(m_ctx);
    ImGuiIO& io = ImGui::GetIO();
    m_server.scheduleFrame();
    if (pressed) {
        if (sym == XKB_KEY_Escape) {
            close();
            return true;
        }
        if (m_mode == Mode::Rename) {
            if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
                m_accept = true;
                return true;
            }
        } else if (m_mode == Mode::Quit) {
            if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_e || sym == XKB_KEY_E)
                m_server.quit();
            return true;
        } else if (m_mode == Mode::Help) {
            close();
            return true;
        }
    }
    // Typed before the card's first frame gave its field the focus: straight
    // into the field (replacing what is there, as the select-all would).
    if (m_focusInput && m_mode == Mode::Rename) {
        char* buf = m_rename;
        const std::size_t cap = sizeof(m_rename);
        if (pressed && !m_typedEarly) {
            buf[0] = 0;
            m_typedEarly = true;
        }
        if (pressed && sym == XKB_KEY_BackSpace) {
            std::size_t n = std::strlen(buf);
            while (n > 0 && (static_cast<unsigned char>(buf[n - 1]) & 0xC0) == 0x80) --n;
            if (n > 0) buf[n - 1] = 0;
        } else if (pressed && utf8 && static_cast<unsigned char>(utf8[0]) >= 0x20 && utf8[0] != 0x7f &&
                   !(mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_LOGO))) {
            std::strncat(buf, utf8, cap - std::strlen(buf) - 1);
        }
        return true;
    }
    io.AddKeyEvent(ImGuiMod_Ctrl, (mods & WLR_MODIFIER_CTRL) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (mods & WLR_MODIFIER_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (mods & WLR_MODIFIER_ALT) != 0);
    const ImGuiKey k = imguiKey(sym);
    if (k != ImGuiKey_None) io.AddKeyEvent(k, pressed);
    if (pressed && utf8 && utf8[0] && static_cast<unsigned char>(utf8[0]) >= 0x20 &&
        utf8[0] != 0x7f && !(mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_LOGO)))
        io.AddInputCharactersUTF8(utf8);
    return true;
}

// --- Cards ------------------------------------------------------------------------------------

void Hud::toast(const std::string& text) {
    if (!m_toasts.empty() && m_toasts.back().text == text && m_toasts.back().age < 1.0) {
        m_toasts.back().age = 0.0;
    } else {
        m_toasts.push_back({text, 0.0});
        if (m_toasts.size() > 3) m_toasts.erase(m_toasts.begin());
    }
    // A note makes no sound of its own: what it reports already clicked.
    m_server.scheduleFrame();
}

void Hud::openLauncher(int room) {
    m_mode = Mode::None;
    m_ring.open(room);
    m_server.refreshPointerFocus();
    m_server.scheduleFrame();
}

void Hud::openRename(int room) {
    m_ring.close();
    m_mode = Mode::Rename;
    m_modeSince = m_time;
    clearToasts();   // the card says which room it is
    m_targetRoom = room;
    std::snprintf(m_rename, sizeof(m_rename), "%s",
                  m_server.desk->rooms[static_cast<std::size_t>(room)].name.c_str());
    m_focusInput = true;
    m_typedEarly = false;
    m_server.sfx->play(Sfx::MenuOpen, 0.6f);
    m_server.scheduleFrame();
}

void Hud::toggleHelp() {
    m_ring.close();
    m_mode = m_mode == Mode::Help ? Mode::None : Mode::Help;
    m_modeSince = m_time;
    m_server.sfx->play(m_mode == Mode::Help ? Sfx::MenuOpen : Sfx::MenuClose, 0.6f);
    m_server.scheduleFrame();
}

void Hud::askQuit() {
    m_ring.close();
    m_mode = Mode::Quit;
    m_modeSince = m_time;
    m_server.sfx->play(Sfx::MenuOpen, 0.6f);
    m_server.scheduleFrame();
}

void Hud::close() {
    if (m_mode != Mode::None) m_server.sfx->play(Sfx::MenuClose, 0.6f);
    m_mode = Mode::None;
    m_accept = false;
    m_ring.close();
    m_server.scheduleFrame();
    m_server.refreshPointerFocus();
}

void Hud::launch(int index, int room) {
    Apps& apps = *m_server.apps;
    if (index < 0 || index >= static_cast<int>(apps.all().size())) return;
    App& app = const_cast<App&>(apps.all()[static_cast<std::size_t>(index)]);
    Desk& desk = *m_server.desk;
    if (room < 0 || room >= static_cast<int>(desk.rooms.size())) room = desk.current;
    if (room != desk.current) desk.goToRoom(room);
    desk.setLevel(Level::Work);
    auto& recent = desk.rooms[static_cast<std::size_t>(room)].recentApps;
    recent.erase(std::remove(recent.begin(), recent.end(), app.id), recent.end());
    recent.insert(recent.begin(), app.id);
    if (recent.size() > 8) recent.resize(8);
    m_server.state->save();
    m_server.spawn(apps.command(app, m_server.onNvidia()));
    toast(app.name + " startet");
}

// --- Drawing helpers ---------------------------------------------------------------------------

void Hud::cardFrame(ImDrawList* dl, float x0, float y0, float x1, float y1, const char* title) {
    // A card: dark glass, a hairline, brackets that come in, a title in caps.
    const float t = theme::easeOut(static_cast<float>((m_time - m_modeSince) / 0.25));
    const float cy = (y0 + y1) * 0.5f;
    const float hy = (y1 - y0) * 0.5f * (0.15f + 0.85f * t);
    const ImVec2 a(x0, cy - hy), b(x1, cy + hy);
    dl->AddRectFilled(a, b, rgba(theme::kPanel, 0.93f * t));
    // Scan lines over the glass, faint.
    for (float y = a.y + 2; y < b.y; y += 4) dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), rgba(theme::kHolo, 0.025f * t));
    const glm::vec3 acc = m_server.desk->rooms[static_cast<std::size_t>(m_server.desk->current)].accent;
    holoFrame(dl, a, b, rgba(theme::kHolo, 0.35f * t), rgba(theme::kHolo, 0.9f * t), 18.0f, t);
    dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(a.x + 4, a.y + 30), rgba(acc, t));
    if (title)
        spacedText(dl, m_fonts.display, 15.0f, ImVec2(a.x + 22, a.y + 16), rgba(theme::kHolo, t), title, 2.5f);
}

void Hud::readSystem() {
    if (m_time - m_sysAt < 2.0) return;
    m_sysAt = m_time;
    std::ifstream st("/proc/stat");
    std::string cpu;
    unsigned long long v[8] = {};
    st >> cpu >> v[0] >> v[1] >> v[2] >> v[3] >> v[4] >> v[5] >> v[6] >> v[7];
    unsigned long long total = 0;
    for (unsigned long long x : v) total += x;
    const unsigned long long idle = v[3] + v[4];
    if (m_cpuTotal && total > m_cpuTotal)
        m_cpu = 1.0f - static_cast<float>(idle - m_cpuIdle) / static_cast<float>(total - m_cpuTotal);
    m_cpuIdle = idle;
    m_cpuTotal = total;
    std::ifstream mi("/proc/meminfo");
    std::string k;
    unsigned long long val = 0, memTotal = 0, memAvail = 0;
    std::string unit;
    while (mi >> k >> val >> unit) {
        if (k == "MemTotal:") memTotal = val;
        else if (k == "MemAvailable:") memAvail = val;
    }
    if (memTotal) m_mem = 1.0f - static_cast<float>(memAvail) / static_cast<float>(memTotal);
}

// --- The strip at the top -----------------------------------------------------------------------

void Hud::drawTopBar(int w) {
    Desk& desk = *m_server.desk;
    if (desk.topBar() == 0) return;
    readSystem();
    const float h = static_cast<float>(desk.topBar());
    const RingMenu::Fonts& f = m_fonts;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(w), h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##top", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const glm::vec3 acc = desk.rooms[static_cast<std::size_t>(desk.current)].accent;
    const glm::vec3 holo = theme::kHolo;
    const float W = static_cast<float>(w);

    // The strip: darker at the top, a line of light along its foot with a
    // brighter stretch under each end.
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, h), IM_COL32(1, 6, 11, 235), IM_COL32(1, 6, 11, 235),
                                IM_COL32(2, 10, 18, 170), IM_COL32(2, 10, 18, 170));
    dl->AddLine(ImVec2(0, h - 1), ImVec2(W, h - 1), rgba(holo, 0.22f));
    dl->AddLine(ImVec2(0, h - 1), ImVec2(330, h - 1), rgba(holo, 0.75f), 1.5f);
    dl->AddLine(ImVec2(330, h - 1), ImVec2(342, h - 7), rgba(holo, 0.75f), 1.5f);
    dl->AddLine(ImVec2(W - 430, h - 1), ImVec2(W, h - 1), rgba(holo, 0.75f), 1.5f);
    dl->AddLine(ImVec2(W - 430, h - 1), ImVec2(W - 442, h - 7), rgba(holo, 0.75f), 1.5f);
    int hoverChip = -2;

    // The logo: a hexagon with a turning arc; it opens the menu.
    const float spin = static_cast<float>(std::fmod(m_time, 600.0));
    const ImVec2 lc(24, h * 0.5f);
    ImGui::SetCursorPos(ImVec2(8, 2));
    const bool logoClicked = ImGui::InvisibleButton("##logo", ImVec2(100, h - 4));
    const bool logoHover = ImGui::IsItemHovered();
    if (logoHover) hoverChip = -1;
    drawGlyph(dl, Glyph::Logo, lc, 10.0f, rgba(logoHover ? theme::kText : holo, 1.0f), 1.5f);
    dl->PathArcTo(lc, 14.0f, spin * 1.5f, spin * 1.5f + 1.6f, 16);
    dl->PathStroke(rgba(acc, 0.9f), ImDrawFlags_None, 1.5f);
    spacedText(dl, f.display, 16.0f, ImVec2(46, h * 0.5f - 8.5f), rgba(logoHover ? holo : theme::kText, 1.0f), "RAUM", 3.5f);
    if (logoClicked) openLauncher();

    // The rooms, as chips.
    float x = 128.0f;
    for (int i = 0; i < static_cast<int>(desk.rooms.size()); ++i) {
        const Room& r = desk.rooms[static_cast<std::size_t>(i)];
        const bool cur = i == desk.current;
        const int count = desk.windowCount(i);
        char num[8];
        std::snprintf(num, sizeof(num), "%02d", i + 1);
        const std::string name = upper(r.name);
        const float nw = spacedWidth(f.display, 13.0f, name.c_str(), 1.5f);
        const float cw = 30.0f + nw + (count ? 26.0f : 8.0f) + 16.0f;
        ImGui::SetCursorPos(ImVec2(x, 7));
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##room", ImVec2(cw, h - 14));
        const bool hov = ImGui::IsItemHovered();
        const bool dbl = hov && ImGui::IsMouseDoubleClicked(0);
        ImGui::PopID();
        if (hov) hoverChip = i;
        const ImVec2 a(x, 7), b(x + cw, h - 7);
        chip(dl, a, b, cur ? rgba(r.accent, 0.22f) : rgba(holo, hov ? 0.10f : 0.0f),
             cur ? rgba(r.accent, 0.95f) : rgba(holo, hov ? 0.6f : 0.25f));
        if (cur) dl->AddLine(ImVec2(a.x + 6, b.y + 3), ImVec2(b.x - 6, b.y + 3), rgba(r.accent, 1.0f), 2.0f);
        dl->AddText(f.mono, 12.0f, ImVec2(x + 12, h * 0.5f - 7), rgba(cur ? r.accent : theme::kDim, 1.0f), num);
        spacedText(dl, f.display, 13.0f, ImVec2(x + 36, h * 0.5f - 7),
                   rgba(cur ? theme::kText : (hov ? holo : theme::kDim), 1.0f), name.c_str(), 1.5f);
        if (count) {
            char c[8];
            std::snprintf(c, sizeof(c), "%d", count);
            hexagon(dl, ImVec2(x + 36 + nw + 16, h * 0.5f), 8.0f, rgba(cur ? r.accent : holo, 0.6f), 1.0f);
            const ImVec2 ts = textSize(f.mono, 10.0f, c);
            dl->AddText(f.mono, 10.0f, ImVec2(x + 36 + nw + 16 - ts.x * 0.5f, h * 0.5f - ts.y * 0.5f),
                        rgba(theme::kText, 0.9f), c);
        }
        if (clicked && !dbl) {
            if (cur && desk.level == Level::Work) desk.setLevel(Level::Room);
            else {
                desk.goToRoom(i);
                desk.setLevel(Level::Work);
            }
        }
        if (dbl) openRename(i);
        x += cw + 10.0f;
    }
    {
        ImGui::SetCursorPos(ImVec2(x, 7));
        const bool clicked = ImGui::InvisibleButton("##newroom", ImVec2(34, h - 14));
        const bool hov = ImGui::IsItemHovered();
        if (hov) hoverChip = 1000;
        chip(dl, ImVec2(x, 7), ImVec2(x + 34, h - 7), rgba(holo, hov ? 0.12f : 0.0f), rgba(holo, hov ? 0.7f : 0.3f));
        drawGlyph(dl, Glyph::NewRoom, ImVec2(x + 17, h * 0.5f), 7.0f, rgba(hov ? theme::kText : holo, 1.0f), 1.2f);
        if (hov) ImGui::SetTooltip("Neuer Raum (Super+N)");
        if (clicked) {
            const int r = desk.newRoom();
            desk.goToRoom(r);
            openRename(r);
        }
    }

    // Right: readouts, the level, sound, the clock.
    const std::time_t t = std::time(nullptr);
    std::tm lt{};
    localtime_r(&t, &lt);
    char hm[16], sec[8], date[32];
    std::snprintf(hm, sizeof(hm), "%02d:%02d", lt.tm_hour, lt.tm_min);
    std::snprintf(sec, sizeof(sec), "%02d", lt.tm_sec);
    std::snprintf(date, sizeof(date), "%s %02d.%02d.", kWeekdays[lt.tm_wday], lt.tm_mday, lt.tm_mon + 1);
    float rx = W - 16.0f;
    // The clock, with the seconds as an arc.
    const ImVec2 hs = textSize(f.mono, 19.0f, hm);
    rx -= hs.x;
    dl->AddText(f.mono, 19.0f, ImVec2(rx, h * 0.5f - hs.y * 0.5f - 1), rgba(theme::kText, 1.0f), hm);
    const ImVec2 sc(rx - 16, h * 0.5f);
    dl->AddCircle(sc, 9.0f, rgba(holo, 0.25f), 24, 1.0f);
    dl->PathArcTo(sc, 9.0f, -kPi / 2, -kPi / 2 + 2 * kPi * (lt.tm_sec / 60.0f), 24);
    dl->PathStroke(rgba(acc, 1.0f), ImDrawFlags_None, 2.0f);
    dl->AddText(f.mono, 8.5f, ImVec2(sc.x - 5, sc.y - 5), rgba(theme::kDim, 1.0f), sec);
    rx = sc.x - 22;
    const float dw = spacedWidth(f.display, 12.0f, date, 1.5f);
    rx -= dw;
    spacedText(dl, f.display, 12.0f, ImVec2(rx, h * 0.5f - 6.5f), rgba(theme::kDim, 1.0f), date, 1.5f);
    rx -= 26;
    // Sound, a switch.
    {
        ImGui::SetCursorPos(ImVec2(rx - 14, 6));
        const bool clicked = ImGui::InvisibleButton("##sound", ImVec2(28, h - 12));
        const bool hov = ImGui::IsItemHovered();
        if (hov) hoverChip = 1001;
        drawGlyph(dl, m_server.sfx->muted() ? Glyph::SoundOff : Glyph::SoundOn, ImVec2(rx, h * 0.5f), 7.5f,
                  rgba(hov ? theme::kText : (m_server.sfx->muted() ? theme::kDim : holo), 1.0f), 1.3f);
        if (hov) ImGui::SetTooltip(m_server.sfx->muted() ? "Ton an (Super+M)" : "Ton aus (Super+M)");
        if (clicked) m_server.toggleSound();
        rx -= 30;
    }
    // The level: three hexagons, the lit one where the camera is.
    {
        const char* lv = kLevels[static_cast<int>(desk.level)];
        const float lw = spacedWidth(f.display, 12.0f, lv, 2.0f);
        rx -= lw;
        spacedText(dl, f.display, 12.0f, ImVec2(rx, h * 0.5f - 6.5f), rgba(holo, 1.0f), lv, 2.0f);
        rx -= 12;
        for (int i = 2; i >= 0; --i) {
            const ImVec2 p(rx - (2 - i) * 13.0f, h * 0.5f);
            if (i == static_cast<int>(desk.level)) hexagon(dl, p, 5.0f, rgba(acc, 1.0f), 0, true);
            hexagon(dl, p, 5.0f, rgba(holo, 0.7f), 1.0f);
        }
        rx -= 2 * 13.0f + 22;
    }
    // Processor and memory: small bars, like an instrument panel.
    auto meter = [&](const char* label, float v) {
        const float bw = 52.0f;
        rx -= bw;
        dl->AddRect(ImVec2(rx, h * 0.5f - 4), ImVec2(rx + bw, h * 0.5f + 4), rgba(holo, 0.4f));
        const int segs = 11;
        for (int s = 0; s < segs; ++s) {
            if (s >= static_cast<int>(std::round(v * segs))) break;
            const float sx = rx + 2 + s * (bw - 4) / segs;
            dl->AddRectFilled(ImVec2(sx, h * 0.5f - 2.5f), ImVec2(sx + (bw - 4) / segs - 1.5f, h * 0.5f + 2.5f),
                              rgba(v > 0.85f ? theme::kWarn : holo, 0.85f));
        }
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%s %2d%%", label, static_cast<int>(v * 100 + 0.5f));
        const ImVec2 ps = textSize(f.mono, 12.0f, pct);
        rx -= ps.x + 8;
        dl->AddText(f.mono, 12.0f, ImVec2(rx, h * 0.5f - ps.y * 0.5f), rgba(theme::kDim, 1.0f), pct);
        rx -= 16;
    };
    meter("MEM", m_mem);
    meter("CPU", m_cpu);
    m_hoverChip = hoverChip;
    ImGui::End();
    ImGui::PopStyleVar();
}

// --- Labels in the room -------------------------------------------------------------------------

void Hud::drawLabels(int w, int h) {
    Desk& desk = *m_server.desk;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const double wall = desk.wallNow();
    const glm::vec3 eye = desk.cameraPos();
    const RingMenu::Fonts& f = m_fonts;

    if (desk.level == Level::Room) {
        // Each window: a callout under it -- its title, and when it was used.
        for (View* v : desk.drawList()) {
            if (v->parentView() || v == desk.dragging) continue;
            const wlr_box g = v->geometry();
            const Pose& p = v->pose;
            const glm::vec3 bottom = p.center - glm::vec3(0, g.height * 0.5f * p.scale + 14.0f, 0);
            const glm::vec3 s = desk.toScreen(bottom);
            if (s.z <= 0 || s.x < -200 || s.x > w + 200 || s.y < 0 || s.y > h + 40) continue;
            glm::vec3 c[4];
            desk.canvasQuad(v, p, c);
            const float pw = glm::length(glm::vec2(desk.toScreen(c[1]) - desk.toScreen(c[0])));
            const float size = std::clamp(pw / 26.0f, 12.0f, 17.0f);
            const Desk::Hit hit = desk.pick(s.x, s.y - 6.0);
            if (hit.view && hit.view != v &&
                glm::length(hit.view->pose.center - eye) < glm::length(p.center - eye))
                continue;
            const float a = std::min(1.0f, p.alpha) * (1.0f - p.dim * 0.5f);
            const bool now = v->slot >= 0;
            const glm::vec3 acc = desk.rooms[static_cast<std::size_t>(v->room)].accent;
            const glm::vec3 col = now ? acc : theme::kHolo;
            const std::string title = fit(f.bold, size, v->title().empty() ? v->appId() : v->title(),
                                          std::max(90.0f, pw * 0.95f));
            const ImVec2 ts = textSize(f.bold, size, title.c_str());
            // The callout: a tick down from the window, a diamond, the text.
            dl->AddLine(ImVec2(s.x, s.y - 10), ImVec2(s.x, s.y + 2), rgba(col, 0.7f * a), 1.0f);
            dl->AddQuadFilled(ImVec2(s.x, s.y + 2), ImVec2(s.x + 4, s.y + 6), ImVec2(s.x, s.y + 10),
                              ImVec2(s.x - 4, s.y + 6), rgba(col, a));
            dl->AddText(f.bold, size, ImVec2(s.x - ts.x * 0.5f, s.y + 13), rgba(theme::kText, a), title.c_str());
            const std::string when = now ? std::string("JETZT") : ago(wall - v->lastFocusWall);
            const ImVec2 ws = textSize(f.mono, size * 0.78f, when.c_str());
            dl->AddText(f.mono, size * 0.78f, ImVec2(s.x - ws.x * 0.5f, s.y + 14 + ts.y), rgba(col, a), when.c_str());
        }
    } else if (desk.hovered && (desk.hovered->slot < 0 || desk.level == Level::House)) {
        // Otherwise only the window under the pointer says what it is.
        View* v = desk.hovered;
        const wlr_box g = v->geometry();
        const glm::vec3 top = v->pose.center + glm::vec3(0, g.height * 0.5f * v->pose.scale + 10.0f, 0);
        const glm::vec3 s = desk.toScreen(top);
        if (s.z > 0) {
            const std::string title = fit(f.bold, 15.0f, v->title(), 480.0f);
            const std::string when = ago(wall - v->lastFocusWall);
            const ImVec2 ts = textSize(f.bold, 15.0f, title.c_str());
            const ImVec2 ws = textSize(f.mono, 11.0f, when.c_str());
            const float bw = ts.x + ws.x + 40;
            const ImVec2 at(std::clamp(s.x - bw * 0.5f, 8.0f, w - bw - 8.0f),
                            std::max(s.y - ts.y - 18.0f, static_cast<float>(desk.topBar()) + 10.0f));
            const ImVec2 a(at.x, at.y), b(at.x + bw, at.y + ts.y + 12);
            dl->AddRectFilled(a, b, rgba(theme::kPanel, 0.9f));
            holoFrame(dl, a, b, rgba(theme::kHolo, 0.35f), rgba(theme::kHolo, 0.9f), 8.0f, 1.0f);
            dl->AddText(f.bold, 15.0f, ImVec2(a.x + 12, a.y + 6), rgba(theme::kText, 1.0f), title.c_str());
            dl->AddText(f.mono, 11.0f, ImVec2(a.x + 24 + ts.x, a.y + 6 + (ts.y - ws.y) * 0.5f),
                        rgba(theme::kHolo, 1.0f), when.c_str());
        }
    }

    if (desk.level == Level::House) {
        // The rooms' names on the floor in front of them, the current one marked.
        for (int i = 0; i < static_cast<int>(desk.rooms.size()); ++i) {
            const Room& r = desk.rooms[static_cast<std::size_t>(i)];
            const glm::vec3 s = desk.toScreen(glm::vec3(desk.roomX(i), desk.floorY(), 260.0f));
            if (s.z <= 0) continue;
            const float size = std::clamp(90000.0f / s.z, 18.0f, 36.0f);
            const bool cur = i == desk.current;
            const std::string name = upper(r.name);
            const float sp = size * 0.18f;
            const float tw = spacedWidth(f.display, size, name.c_str(), sp);
            spacedText(dl, f.display, size, ImVec2(s.x - tw * 0.5f, s.y), rgba(cur ? theme::kText : theme::kDim, 0.95f),
                       name.c_str(), sp);
            const int n = desk.windowCount(i);
            char sub[32];
            if (n == 0) std::snprintf(sub, sizeof(sub), "LEER");
            else std::snprintf(sub, sizeof(sub), "%02d FENSTER", n);
            const ImVec2 ss = textSize(f.mono, size * 0.42f, sub);
            dl->AddText(f.mono, size * 0.42f, ImVec2(s.x - ss.x * 0.5f, s.y + size + 4), rgba(r.accent, 0.9f), sub);
            const float bar = tw * (cur ? 1.0f : 0.35f);
            dl->AddRectFilled(ImVec2(s.x - bar * 0.5f, s.y - 10), ImVec2(s.x + bar * 0.5f, s.y - 7),
                              rgba(r.accent, cur ? 1.0f : 0.6f));
            if (cur) holoFrame(dl, ImVec2(s.x - tw * 0.5f - 18, s.y - 18), ImVec2(s.x + tw * 0.5f + 18, s.y + size + ss.y + 14),
                               rgba(r.accent, 0.0f), rgba(r.accent, 0.8f), 10.0f, 1.0f);
        }
    }

    if (desk.dragging) {
        // The window being carried to another room, under the pointer.
        View* v = desk.dragging;
        if (v->canvas.valid()) {
            const float tw = 260.0f;
            const float th = tw * v->canvas.height / std::max(1, v->canvas.width);
            const ImVec2 at(static_cast<float>(desk.dragPointer.x) - tw * 0.5f,
                            static_cast<float>(desk.dragPointer.y) - th * 0.5f);
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            fg->AddImage(ImTextureRef(static_cast<ImTextureID>(m_server.gpu->device().imguiTextureId(v->canvas.color))),
                         at, ImVec2(at.x + tw, at.y + th), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(200, 240, 255, 230));
            holoFrame(fg, at, ImVec2(at.x + tw, at.y + th), rgba(theme::kHolo, 0.5f), rgba(theme::kHolo, 1.0f), 14.0f, 1.0f);
            const int over = desk.pickRoom(desk.dragPointer.x, desk.dragPointer.y);
            if (over >= 0) {
                const std::string t = "→ " + upper(desk.rooms[static_cast<std::size_t>(over)].name);
                spacedText(fg, m_fonts.display, 16.0f, ImVec2(at.x, at.y + th + 10), rgba(theme::kText, 1.0f), t.c_str(), 2.0f);
            }
        }
    }
}

void Hud::drawWarp(int w, int h) {
    // Streaks of light while the camera flies: sideways between rooms, out of
    // the middle when it leans back or comes in.
    const Desk& desk = *m_server.desk;
    const glm::vec3 v = desk.cameraVelocity();
    const float side = std::abs(v.x), depth = std::sqrt(v.y * v.y + v.z * v.z);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float t = static_cast<float>(std::fmod(m_time, 1000.0));
    if (side > 1200.0f) {
        const float k = std::clamp((side - 1200.0f) / 6000.0f, 0.0f, 1.0f);
        const float dir = v.x > 0 ? -1.0f : 1.0f;
        for (int i = 0; i < 46; ++i) {
            const float hy = std::fmod(std::sin(i * 12.9898f) * 43758.5453f, 1.0f);
            const float hx = std::fmod(std::sin(i * 78.233f) * 12345.678f, 1.0f);
            const float y = (0.5f + 0.5f * hy) * h;
            const float len = (120.0f + 520.0f * std::abs(hx)) * k;
            const float x = std::fmod(std::abs(hx) * w + dir * t * 3000.0f * (0.6f + std::abs(hy)), static_cast<float>(w) + len);
            const float xx = dir > 0 ? x - len : w - x;
            dl->AddLine(ImVec2(xx, y), ImVec2(xx + len, y), rgba(theme::kHolo, 0.6f * k * (0.4f + 0.6f * std::abs(hx))), 1.5f);
        }
    }
    if (depth > 1500.0f && desk.level != Level::Work) {
        const float k = std::clamp((depth - 1500.0f) / 9000.0f, 0.0f, 0.8f);
        const ImVec2 c(w * 0.5f, h * 0.5f);
        for (int i = 0; i < 60; ++i) {
            const float a = i * 2.39996f;
            const float r0 = (0.2f + 0.8f * std::fmod(i * 0.618034f + t * 0.9f, 1.0f)) * w * 0.55f;
            const float len = 40.0f + 160.0f * k;
            const ImVec2 p0(c.x + std::cos(a) * r0, c.y + std::sin(a) * r0);
            const ImVec2 p1(c.x + std::cos(a) * (r0 + len), c.y + std::sin(a) * (r0 + len));
            dl->AddLine(p0, p1, rgba(theme::kHolo, 0.22f * k), 1.0f);
        }
    }
}

// --- The cards -------------------------------------------------------------------------------

void Hud::drawRename(int w, int h) {
    const float pw = 460.0f, ph = 150.0f;
    const float x0 = (w - pw) * 0.5f, y0 = h * 0.3f;
    cardFrame(ImGui::GetBackgroundDrawList(), x0, y0, x0 + pw, y0 + ph, "RAUM BENENNEN");
    ImGui::SetNextWindowPos(ImVec2(x0, y0 + 46));
    ImGui::SetNextWindowSize(ImVec2(pw, ph - 46));
    ImGui::Begin("##rename", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    ImGui::SetCursorPos(ImVec2(22, 4));
    ImGui::PushFont(m_fonts.text, 22.0f);
    ImGui::SetNextItemWidth(pw - 44.0f);
    if (m_focusInput) {
        ImGui::SetKeyboardFocusHere();
        m_focusInput = false;
    }
    ImGui::InputText("##name", m_rename, sizeof(m_rename), ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopFont();
    ImGui::SetCursorPosX(22);
    ImGui::PushFont(m_fonts.mono, 11.0f);
    ImGui::TextDisabled("ENTER ÜBERNIMMT  ·  ESC BRICHT AB");
    ImGui::PopFont();
    ImGui::End();
    if (m_accept) {
        m_accept = false;
        m_server.desk->renameRoom(m_targetRoom, m_rename);
        m_server.sfx->play(Sfx::Launch, 0.5f);
        m_mode = Mode::None;
        m_server.refreshPointerFocus();
    }
}

void Hud::drawHelp(int w, int h) {
    static const char* rows[][2] = {
        {"SUPER  LEERTASTE", "Menü: Programme und raums Aktionen"},
        {"SUPER  ENTER", "Terminal"},
        {"SUPER  TAB", "das Fenster dahinter nach vorn"},
        {"SUPER  ⇧  TAB", "das älteste Fenster nach vorn"},
        {"KLICK DAHINTER", "holt ein Fenster nach vorn"},
        {"SUPER (KURZ)", "zurücklehnen: Arbeit ↔ Raum"},
        {"SUPER  ↑ ↓", "Arbeit → Raum → Haus"},
        {"SUPER  ← →   1…9", "Raum wechseln"},
        {"SUPER  ⇧  ← →  1…9", "Fenster in einen anderen Raum"},
        {"SUPER  N  /  R", "neuer Raum / umbenennen"},
        {"SUPER  P", "2, 3, 1 Fenster nebeneinander"},
        {"SUPER  F  /  ⇧ F", "maximieren / Vollbild"},
        {"SUPER  F11", "raum im Vollbild / im Fenster"},
        {"SUPER  ZIEHEN", "Fenster verschieben (rechts: Größe)"},
        {"SUPER  Q", "Fenster schließen"},
        {"SUPER  S", "Bildschirmfoto nach ~/Bilder"},
        {"SUPER  M", "Ton an / aus"},
        {"SUPER  ⇧  E", "raum beenden"},
    };
    const int n = static_cast<int>(sizeof(rows) / sizeof(rows[0]));
    const float pw = std::min(760.0f, w - 40.0f), ph = 130.0f + n * 27.0f;
    const float x0 = (w - pw) * 0.5f, y0 = std::max(60.0f, (h - ph) * 0.45f);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    cardFrame(dl, x0, y0, x0 + pw, y0 + ph, "RAUM  ·  BEDIENUNG");
    const float t = theme::easeOut(static_cast<float>((m_time - m_modeSince) / 0.3));
    dl->AddText(m_fonts.text, 14.0f, ImVec2(x0 + 22, y0 + 44), rgba(theme::kDim, t),
                "Vorn wird gearbeitet. Dahinter wartet, was du zuletzt benutzt hast -- je länger her, desto weiter hinten.");
    for (int i = 0; i < n; ++i) {
        const float rt = std::clamp((static_cast<float>(m_time - m_modeSince) - 0.08f - i * 0.012f) / 0.15f, 0.0f, 1.0f);
        const float y = y0 + 80 + i * 27.0f;
        const ImVec2 ks = textSize(m_fonts.mono, 12.0f, rows[i][0]);
        const ImVec2 ka(x0 + 22, y), kb(x0 + 32 + ks.x, y + 21);
        dl->AddRect(ka, kb, rgba(theme::kHolo, 0.55f * rt));
        dl->AddText(m_fonts.mono, 12.0f, ImVec2(ka.x + 5, ka.y + 3), rgba(theme::kHolo, rt), rows[i][0]);
        dl->AddText(m_fonts.text, 14.5f, ImVec2(x0 + 250, y + 2), rgba(theme::kText, rt), rows[i][1]);
    }
    dl->AddText(m_fonts.mono, 11.0f, ImVec2(x0 + 22, y0 + ph - 26), rgba(theme::kDim, t),
                "EINE BELIEBIGE TASTE SCHLIESST DIESE KARTE");
    // ImGui has to know the pointer is on the card.
    ImGui::SetNextWindowPos(ImVec2(x0, y0));
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::Begin("##help", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0)) close();
    ImGui::End();
}

void Hud::drawQuit(int w, int h) {
    const float pw = 440.0f, ph = 170.0f;
    const float x0 = (w - pw) * 0.5f, y0 = h * 0.32f;
    cardFrame(ImGui::GetBackgroundDrawList(), x0, y0, x0 + pw, y0 + ph, "RAUM BEENDEN?");
    ImGui::SetNextWindowPos(ImVec2(x0, y0 + 44));
    ImGui::SetNextWindowSize(ImVec2(pw, ph - 44));
    ImGui::Begin("##quit", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    ImGui::SetCursorPos(ImVec2(22, 6));
    ImGui::PushFont(m_fonts.text, 15.0f);
    ImGui::TextDisabled("Alle Programme darin werden geschlossen.");
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(22, 50));
    ImGui::PushFont(m_fonts.display, 14.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, vec4(theme::kWarn, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_Text, vec4(theme::kWarn, 1.0f));
    if (ImGui::Button("BEENDEN  (ENTER)")) m_server.quit();
    ImGui::PopStyleColor(2);
    ImGui::SameLine(0, 14);
    if (ImGui::Button("BLEIBEN  (ESC)")) close();
    ImGui::PopFont();
    ImGui::End();
}

void Hud::drawToasts(int w, int h) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float y = h - 74.0f;
    const glm::vec3 acc = m_server.desk->rooms[static_cast<std::size_t>(m_server.desk->current)].accent;
    for (auto it = m_toasts.rbegin(); it != m_toasts.rend(); ++it) {
        const float age = static_cast<float>(it->age);
        const float a = std::clamp((2.2f - age) / 0.3f, 0.0f, 1.0f);
        const float wipe = theme::easeOut(age / 0.22f);
        const std::string text = upper(it->text);
        const float tw = spacedWidth(m_fonts.display, 15.0f, text.c_str(), 2.0f);
        const float bw = tw + 64.0f, bh = 34.0f;
        const float x0 = (w - bw) * 0.5f;
        const ImVec2 A(x0, y), B(x0 + bw * wipe, y + bh);
        dl->PushClipRect(A, ImVec2(B.x + 12, B.y + 1), true);
        chip(dl, A, ImVec2(x0 + bw, y + bh), rgba(theme::kPanel, 0.92f * a), rgba(theme::kHolo, 0.7f * a));
        hexagon(dl, ImVec2(x0 + 24, y + bh * 0.5f), 6.0f, rgba(acc, a), 0, true);
        spacedText(dl, m_fonts.display, 15.0f, ImVec2(x0 + 40, y + bh * 0.5f - 8), rgba(theme::kText, a), text.c_str(), 2.0f);
        dl->PopClipRect();
        // The scan line that wrote it.
        if (wipe < 1.0f) dl->AddLine(ImVec2(B.x, y - 3), ImVec2(B.x, y + bh + 3), rgba(theme::kHolo, 1.0f - wipe), 2.0f);
        y -= bh + 12.0f;
    }
}

void Hud::drawEmptyRoom(int w, int h) {
    // An empty room greets with what used to be in it: one click sets it up again.
    Desk& desk = *m_server.desk;
    if (desk.level != Level::Work || desk.windowCount(desk.current) > 0 || m_ring.isOpen()) return;
    Apps& apps = *m_server.apps;
    const Room& room = desk.rooms[static_cast<std::size_t>(desk.current)];
    std::vector<App*> recent;
    for (const std::string& id : room.recentApps)
        if (App* a = apps.find(id)) recent.push_back(a);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float cx = w * 0.5f, y = h * 0.30f;
    const std::string name = upper(room.name);
    const float tw = spacedWidth(m_fonts.display, 40.0f, name.c_str(), 8.0f);
    spacedText(dl, m_fonts.display, 40.0f, ImVec2(cx - tw * 0.5f, y), rgba(theme::kText, 0.95f), name.c_str(), 8.0f);
    dl->AddRectFilled(ImVec2(cx - 30, y - 16), ImVec2(cx + 30, y - 13), rgba(room.accent, 1.0f));
    const char* sub = recent.empty() ? "EIN LEERER RAUM  ·  SUPER + LEERTASTE ÖFFNET DAS MENÜ" : "HIER WAR ZULETZT";
    const ImVec2 ss = textSize(m_fonts.mono, 13.0f, sub);
    dl->AddText(m_fonts.mono, 13.0f, ImVec2(cx - ss.x * 0.5f, y + 56), rgba(theme::kHolo, 0.85f), sub);
    if (recent.empty()) return;
    const int n = std::min<int>(6, static_cast<int>(recent.size()));
    const float step = 110.0f;
    ImGui::SetNextWindowPos(ImVec2(cx - n * step * 0.5f, y + 90));
    ImGui::SetNextWindowSize(ImVec2(n * step, 130));
    ImGui::Begin("##empty", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                         ImGuiWindowFlags_NoBringToFrontOnFocus);
    for (int i = 0; i < n; ++i) {
        App* a = recent[static_cast<std::size_t>(i)];
        const ImVec2 c(cx - n * step * 0.5f + step * (i + 0.5f), y + 140);
        ImGui::SetCursorScreenPos(ImVec2(c.x - 44, c.y - 44));
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##app", ImVec2(88, 110));
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        hexagon(dl, c, 38.0f, rgba(hov ? room.accent : theme::kHoloDeep, hov ? 0.25f : 0.18f), 0, true);
        hexagon(dl, c, 38.0f, rgba(hov ? room.accent : theme::kHolo, hov ? 1.0f : 0.45f), hov ? 2.0f : 1.2f);
        if (fitzel::Texture* t = apps.icon(*a))
            dl->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(m_server.gpu->device().imguiTextureId(t->id()))),
                                ImVec2(c.x - 22, c.y - 22), ImVec2(c.x + 22, c.y + 22), ImVec2(0, 0), ImVec2(1, 1),
                                IM_COL32_WHITE, 6.0f);
        const std::string nm = fit(m_fonts.text, 13.0f, a->name, step - 10);
        const ImVec2 ns = textSize(m_fonts.text, 13.0f, nm.c_str());
        dl->AddText(m_fonts.text, 13.0f, ImVec2(c.x - ns.x * 0.5f, c.y + 46), rgba(hov ? theme::kText : theme::kDim, 1.0f),
                    nm.c_str());
        if (clicked) {
            for (int k = 0; k < static_cast<int>(apps.all().size()); ++k)
                if (&apps.all()[static_cast<std::size_t>(k)] == a) {
                    m_server.sfx->play(Sfx::Launch);
                    launch(k, desk.current);
                    break;
                }
        }
    }
    ImGui::End();
}

// --- The frame ------------------------------------------------------------------------------

void Hud::render(int width, int height, double dt) {
    ImGui::SetCurrentContext(m_ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DeltaTime = static_cast<float>(std::clamp(dt, 1e-4, 0.25));
    m_time = m_server.desk->now();
    for (Toast& t : m_toasts) t.age += dt;
    m_toasts.erase(std::remove_if(m_toasts.begin(), m_toasts.end(),
                                  [](const Toast& t) { return t.age > 2.2; }),
                   m_toasts.end());

    m_server.gpu->device().imguiNewFrame();
    ImGui::NewFrame();
    drawWarp(width, height);
    drawLabels(width, height);
    if (m_mode == Mode::None) drawEmptyRoom(width, height);
    drawTopBar(width);
    switch (m_mode) {
        case Mode::Rename: drawRename(width, height); break;
        case Mode::Help: drawHelp(width, height); break;
        case Mode::Quit: drawQuit(width, height); break;
        case Mode::None: break;
    }
    m_ring.draw(ImGui::GetForegroundDrawList(), m_fonts, width, height, m_time);
    drawToasts(width, height);
    ImGui::Render();
    m_server.gpu->device().imguiRender(ImGui::GetDrawData());
}

} // namespace raum
